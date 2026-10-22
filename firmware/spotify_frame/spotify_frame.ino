#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <JC3248W535.h>
#include <Preferences.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>
#include "config.h"
#include "bitmap_font.h"

JC3248W535_Display display;
JC3248W535_Touch touch;
JPEGDEC jpeg;
Preferences preferences;
uint8_t *art = nullptr;
size_t artSize = 0;
String artUrl;
String topNames[3], topImageUrls[3], topArtistStatus = "Loading artists...";
uint8_t *topImages[3] = {nullptr, nullptr, nullptr};
size_t topImageSizes[3] = {0, 0, 0};
uint32_t topArtistsFetchedAt = 0, topArtistsAttemptedAt = 0;
int topArtistCount = 0;
String accessToken, refreshToken, trackId, syncedLyrics;
String lyricAlbum;
bool lyricRetryPending = false;
uint32_t lastLyricsAttempt = 0;
String prefetchedTrackId, prefetchedLyrics, prefetchedStatus, prefetchAttemptedForTrack;
uint32_t tokenExpiresAt = 0, polledAt = 0;
volatile uint32_t spotifyRetryAt = 0;
uint32_t playbackRequestCount = 0, queueRequestCount = 0, topArtistRequestCount = 0;
bool spotifyRateLimited();
bool touchReady = false, touchWasDown = false, tapRefreshPending = false;
uint32_t lastTapAt = 0, tapFeedbackUntil = 0;
const char *tapFeedback = nullptr;
uint32_t prefetchGeneration = 0;
SemaphoreHandle_t prefetchMutex;
volatile bool prefetchBusy = false;

bool playing = false;
bool isPlaying = false;
constexpr uint32_t IDLE_DELAY_MS = 300000;
constexpr uint32_t PLAYING_POLL_MS = 10000;
constexpr uint32_t IDLE_POLL_MS = 30000;
constexpr uint8_t NIGHT_BACKLIGHT = 40; // 0-255; lower this if the clock is still too bright.
uint32_t inactiveSince = 0;
String title, artist, lyric, nextLyric;
uint32_t progressMs = 0, durationMs = 0;

int drawJpeg(JPEGDRAW *block) {
  display.getCanvas()->draw16bitRGBBitmap(
      block->x, block->y, block->pPixels, block->iWidth, block->iHeight);
  return 1;
}

uint32_t nextCodepoint(const String &text, int &at) {
  uint8_t first = text[at++];
  if (first < 0x80) return first;
  int extra = (first & 0xE0) == 0xC0 ? 1 : (first & 0xF0) == 0xE0 ? 2 :
              (first & 0xF8) == 0xF0 ? 3 : 0;
  if (!extra || at + extra > text.length()) return '?';
  uint32_t code = first & (0x7F >> (extra + 1));
  while (extra--) code = (code << 6) | ((uint8_t)text[at++] & 0x3F);
  return code;
}

bool isMark(uint32_t code) { return code >= 0x591 && code <= 0x5C7; }

BitmapGlyph glyphFor(uint32_t code, int size) {
  if (code == 0x02BC || code == 0xFF07) code = '\'';
  const BitmapGlyph *table = size == 30 ? font30Glyphs :
                             size == 24 ? font24Glyphs : font18Glyphs;
  int high = (size == 30 ? sizeof(font30Glyphs) :
              size == 24 ? sizeof(font24Glyphs) : sizeof(font18Glyphs)) /
             sizeof(BitmapGlyph) - 1;
  int low = 0;
  while (low <= high) {
    int mid = (low + high) / 2;
    BitmapGlyph glyph;
    memcpy_P(&glyph, table + mid, sizeof(glyph));
    if (glyph.code == code) return glyph;
    if (glyph.code < code) low = mid + 1;
    else high = mid - 1;
  }
  return code == '?' ? BitmapGlyph{63, 0, 0, 0} : glyphFor('?', size);
}

int textWidth(const String &value, int size) {
  int width = 0;
  for (int at = 0; at < value.length();) {
    uint32_t code = nextCodepoint(value, at);
    if (!isMark(code)) width += glyphFor(code, size).advance;
  }
  return width;
}

bool hasHebrew(const String &value) {
  for (int at = 0; at < value.length();) {
    uint32_t code = nextCodepoint(value, at);
    if (code >= 0x5D0 && code <= 0x5EA) return true;
  }
  return false;
}

void drawGlyph(uint32_t code, int x, int y, int size, uint16_t color) {
  if (isMark(code)) return;
  BitmapGlyph glyph = glyphFor(code, size);
  const uint8_t *bits = (size == 30 ? font30Bits :
                         size == 24 ? font24Bits : font18Bits) + glyph.offset;
  int height = size == 30 ? font30Height : size == 24 ? font24Height : font18Height;
  int stride = (glyph.width + 7) / 8;
  auto gfx = display.getCanvas();
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < glyph.width; ++col) {
      if (pgm_read_byte(bits + row * stride + col / 8) & (0x80 >> (col % 8)))
        gfx->drawPixel(x + col, y + row, color);
    }
  }
}

void drawLine(const String &value, int x, int y, int width, int size, uint16_t color) {
  if (hasHebrew(value)) {
    int right = x + width;
    for (int start = 0; start < value.length();) {
      int end = value.indexOf(' ', start);
      if (end < 0) end = value.length();
      String word = value.substring(start, end);
      if (hasHebrew(word)) {
        for (int at = 0; at < word.length();) {
          uint32_t code = nextCodepoint(word, at);
          if (code == '(') code = ')';
          else if (code == ')') code = '(';
          if (!isMark(code)) { right -= glyphFor(code, size).advance; drawGlyph(code, right, y, size, color); }
        }
      } else {
        right -= textWidth(word, size);
        for (int at = 0; at < word.length();) {
          uint32_t code = nextCodepoint(word, at);
          drawGlyph(code, right, y, size, color);
          right += glyphFor(code, size).advance;
        }
        right -= textWidth(word, size);
      }
      if (end == value.length()) break;
      right -= glyphFor(' ', size).advance;
      start = end + 1;
    }
  } else {
    for (int at = 0; at < value.length();) {
      uint32_t code = nextCodepoint(value, at);
      if (!isMark(code)) { drawGlyph(code, x, y, size, color); x += glyphFor(code, size).advance; }
    }
  }
}

int wrapText(const String &value, int width, int size, String lines[], int capacity) {
  int count = 0;
  String current;
  for (int start = 0; start < value.length() && count < capacity;) {
    while (start < value.length() && value[start] == ' ') ++start;
    int end = value.indexOf(' ', start);
    if (end < 0) end = value.length();
    String word = value.substring(start, end);
    String candidate = current.length() ? current + " " + word : word;
    if (textWidth(candidate, size) <= width) current = candidate;
    else {
      if (current.length()) { lines[count++] = current; current = ""; }
      for (int at = 0; at < word.length() && count < capacity;) {
        int begin = at;
        nextCodepoint(word, at);
        String candidatePart = current + word.substring(begin, at);
        if (current.length() && textWidth(candidatePart, size) > width) {
          lines[count++] = current;
          current = "";
        }
        current += word.substring(begin, at);
      }
    }
    start = end + 1;
  }
  if (current.length() && count < capacity) lines[count++] = current;
  return count;
}

int drawWrapped(const String &value, int x, int y, int width, int size,
                uint16_t color, int maxLines, bool scroll = false) {
  String lines[24];
  int count = wrapText(value, width, size, lines, 24);
  int first = scroll && count > maxLines ? (millis() / 2500) % (count - maxLines + 1) : 0;
  for (int i = 0; i < maxLines && first + i < count; ++i)
    drawLine(lines[first + i], x, y + i * (size == 30 ? 37 : size == 24 ? 30 : 23), width, size, color);
  return min(count, maxLines);
}

void drawClock() {
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char digits[6];
  strftime(digits, sizeof(digits), "%H:%M", &local);
  const int scale = 3;
  int x = (480 - textWidth(digits, 30) * scale) / 2;
  auto gfx = display.getCanvas();
  for (int at = 0; digits[at]; ++at) {
    BitmapGlyph glyph = glyphFor(digits[at], 30);
    const uint8_t *bits = font30Bits + glyph.offset;
    int stride = (glyph.width + 7) / 8;
    for (int row = 0; row < font30Height; ++row)
      for (int col = 0; col < glyph.width; ++col)
        if (pgm_read_byte(bits + row * stride + col / 8) & (0x80 >> (col % 8)))
          gfx->fillRect(x + col * scale, 30 + row * scale, scale, scale, 0xFFFF);
    x += glyph.advance * scale;
  }
}

void renderIdle() {
  auto gfx = display.getCanvas();
  gfx->fillScreen(0x0841);
  gfx->fillRoundRect(8, 8, 464, 304, 16, 0x18C3);
  drawClock();
  gfx->fillRect(22, 146, 436, 1, 0x4A49);
  drawLine(tapFeedback ? tapFeedback : "YOUR TOP ARTISTS", 22, 155, 250, 18, 0x1EA8);
  String period = spotifyRateLimited()
      ? "QUOTA RETRY " + String((spotifyRetryAt - millis() + 59999UL) / 60000UL) + "m"
      : "LAST 4 WEEKS";
  drawLine(period, 458 - textWidth(period, 18), 155,
           textWidth(period, 18), 18, 0xBDF7);
  if (!topArtistCount) {
    drawLine(spotifyRateLimited() ? "Top artists paused" : topArtistStatus,
             22, 225, 436, 24, 0xBDF7);
  } else {
    for (int i = 0; i < topArtistCount; ++i) {
      int left = 22 + i * 148;
      int imageX = left + 29;
      gfx->fillRoundRect(imageX - 3, 184, 94, 94, 7, 0x2945);
      if (topImages[i] && jpeg.openRAM(topImages[i], topImageSizes[i], drawJpeg)) {
        int scale = jpeg.getWidth() > 160 ? JPEG_SCALE_QUARTER :
                    jpeg.getWidth() > 90 ? JPEG_SCALE_HALF : 0;
        int divisor = scale == JPEG_SCALE_QUARTER ? 4 : scale == JPEG_SCALE_HALF ? 2 : 1;
        int width = jpeg.getWidth() / divisor;
        int height = jpeg.getHeight() / divisor;
        if (width <= 88 && height <= 88)
          jpeg.decode(imageX + (88 - width) / 2, 187 + (88 - height) / 2, scale);
        jpeg.close();
      }
      String rank = String(i + 1);
      drawLine(rank, left + 5, 184, 20, 18, 0x1EA8);
      String lines[4];
      int count = wrapText(topNames[i], 138, 18, lines, 4);
      int first = count > 1 ? (millis() / 3000) % count : 0;
      if (count) drawLine(lines[first], left, 282, 138, 18, 0xFFFF);
    }
  }
  display.flush();
}

void render() {
  if (!playing || (inactiveSince && millis() - inactiveSince >= IDLE_DELAY_MS)) {
    renderIdle(); return;
  }
  auto gfx = display.getCanvas();
  gfx->fillScreen(0x0841);
  gfx->fillRoundRect(8, 8, 464, 304, 16, 0x18C3);
  gfx->fillRoundRect(17, 17, 162, 162, 8, 0x2945);

  if (art && jpeg.openRAM(art, artSize, drawJpeg)) {
    int scale = jpeg.getWidth() > 320 ? JPEG_SCALE_QUARTER
              : jpeg.getWidth() > 160 ? JPEG_SCALE_HALF : 0;
    int divisor = scale == JPEG_SCALE_QUARTER ? 4 : scale == JPEG_SCALE_HALF ? 2 : 1;
    jpeg.decode(18 + (160 - jpeg.getWidth() / divisor) / 2,
                18 + (160 - jpeg.getHeight() / divisor) / 2, scale);
    jpeg.close();
  }

  drawLine(tapFeedback ? tapFeedback : (isPlaying ? "NOW PLAYING" : "PAUSED"),
           194, 19, 266, 18, 0x1EA8);
  drawWrapped(title, 194, 50, 266, 24, 0xFFFF, 2, true);
  drawWrapped(artist, 194, 116, 266, 24, 0xBDF7, 1, true);

  gfx->fillRoundRect(194, 151, 266, 5, 2, 0x4A49);
  if (durationMs) {
    int width = (uint64_t)min(progressMs, durationMs) * 266 / durationMs;
    if (width) gfx->fillRoundRect(194, 151, width, 5, 2, 0xFFFF);
  }
  drawLine(String(progressMs / 60000) + ":" +
           (progressMs / 1000 % 60 < 10 ? "0" : "") + String(progressMs / 1000 % 60),
           194, 159, 80, 18, 0xBDF7);
  String duration = String(durationMs / 60000) + ":" +
           (durationMs / 1000 % 60 < 10 ? "0" : "") + String(durationMs / 1000 % 60);
  drawLine(duration, 460 - textWidth(duration, 18), 159, textWidth(duration, 18), 18, 0xBDF7);
  String currentLine = lyric.length() ? lyric : "[Music]";
  String trial[24];
  int lyricSize = wrapText(currentLine, 440, 30, trial, 24) <= 2 ? 30 : 24;
  int shown = drawWrapped(currentLine, 20, 187, 440, lyricSize, 0xFFFF,
                          lyricSize == 30 ? 2 : 3, true);
  int nextY = 187 + shown * (lyricSize == 30 ? 37 : 30) + 9;
  if (nextY <= 278 && nextLyric.length())
    drawWrapped(nextLyric, 20, nextY, 440, 24, 0xBDF7,
                min(2, (307 - nextY) / 30), true);
  display.flush();
}

bool downloadJpeg(const String &url, uint8_t *&target, size_t &targetSize) {
  if (target) { free(target); target = nullptr; targetSize = 0; }
  if (!url.length()) return false;
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != 200) { Serial.printf("Image HTTP %d\n", status); http.end(); return false; }
  if (http.getSize() > 200000) { Serial.println("Image too large"); http.end(); return false; }
  String bytes = http.getString(); // Handles chunked responses and missing Content-Length.
  if (bytes.length() && bytes.length() <= 200000) {
    target = (uint8_t *)ps_malloc(bytes.length());
    if (target) { memcpy(target, bytes.c_str(), bytes.length()); targetSize = bytes.length(); }
  }
  http.end();
  return target != nullptr;
}

void loadArtwork() {
  if (!downloadJpeg(artUrl, art, artSize) && artUrl.length())
    Serial.println("Artwork download failed");
}

String urlEncode(const String &input) {
  const char *hex = "0123456789ABCDEF";
  String output;
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = input[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      output += (char)c;
    } else {
      output += '%'; output += hex[c >> 4]; output += hex[c & 15];
    }
  }
  return output;
}

bool refreshAccessToken() {
  for (int attempt = 0; attempt < 2; ++attempt) {
    NetworkClientSecure client;
    client.useBuiltinCACertBundle();
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
    http.setAuthorization(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int status = http.POST("grant_type=refresh_token&refresh_token=" + urlEncode(refreshToken));
    String body = http.getString();
    http.end();
    if (status != 200) {
      JsonDocument failure;
      deserializeJson(failure, body);
      String code = failure["error"].as<String>();
      Serial.printf("Spotify token HTTP %d: %s\n", status, code.c_str());
      if (status == 400 && attempt == 0 && refreshToken != SPOTIFY_REFRESH_TOKEN) {
        refreshToken = SPOTIFY_REFRESH_TOKEN;
        Serial.println("Retrying token from config.h");
        continue;
      }
      return false;
    }
    JsonDocument response;
    DeserializationError error = deserializeJson(response, body);
    if (error) { Serial.printf("Spotify token JSON error: %s\n", error.c_str()); return false; }
    accessToken = response["access_token"].as<String>();
    uint32_t expiresIn = response["expires_in"] | 3600;
    tokenExpiresAt = millis() + (expiresIn > 60 ? expiresIn - 60 : 60) * 1000UL;
    String newRefreshToken = response["refresh_token"].as<String>();
    if (newRefreshToken.length()) refreshToken = newRefreshToken;
    if (newRefreshToken.length() || attempt > 0) preferences.putString("refresh", refreshToken);
    return accessToken.length() > 0;
  }
  return false;
}

bool spotifyRateLimited() {
  uint32_t retryAt = spotifyRetryAt;
  if (!retryAt) return false;
  if ((int32_t)(millis() - retryAt) < 0) return true;
  if (spotifyRetryAt == retryAt) spotifyRetryAt = 0;
  return false;
}

void noteSpotifyRateLimit(HTTPClient &http) {
  String retryAfter = http.header("Retry-After");
  String body = http.getString();
  JsonDocument error;
  if (!deserializeJson(error, body)) {
    String reason = error["error"]["reason"].as<String>();
    if (reason.length()) Serial.printf("Spotify 429 reason: %s\n", reason.c_str());
  }
  Serial.printf("Spotify 429 Retry-After: %s\n",
                retryAfter.length() ? retryAfter.c_str() : "missing");
  long seconds = retryAfter.toInt();
  if (seconds <= 0) seconds = 60;
  if (seconds > 604800) seconds = 604800; // millis() comparisons support up to 24 days.
  spotifyRetryAt = millis() + seconds * 1000UL;
  Serial.printf("Spotify rate limited; retrying in %ld seconds\n", seconds);
}

bool fetchTopArtists() {
  if (spotifyRateLimited()) return false;
  if (!accessToken.length() || (int32_t)(millis() - tokenExpiresAt) >= 0)
    if (!refreshAccessToken()) return false;
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client,
      "https://api.spotify.com/v1/me/top/artists?limit=3&time_range=short_term")) return false;
  const char *headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Authorization", "Bearer " + accessToken);
  int status = http.GET();
  Serial.printf("Spotify request top-artists #%lu uptime=%lu s HTTP %d\n",
                (unsigned long)++topArtistRequestCount,
                (unsigned long)(millis() / 1000), status);
  if (status != 200) {
    if (status == 429) noteSpotifyRateLimit(http);
    Serial.printf("Spotify top artists HTTP %d\n", status);
    topArtistStatus = status == 403 ? "Top artists need access" : "Artists unavailable";
    http.end();
    if (status == 401) accessToken = "";
    if (status == 403 && refreshToken != SPOTIFY_REFRESH_TOKEN) {
      // A stored token may predate the user-top-read permission in config.h.
      refreshToken = SPOTIFY_REFRESH_TOKEN;
      preferences.putString("refresh", refreshToken);
      accessToken = "";
      topArtistsAttemptedAt = 0;
    }
    return false;
  }
  JsonDocument filter;
  filter["items"][0]["name"] = true;
  filter["items"][0]["images"][0]["url"] = true;
  filter["items"][0]["images"][0]["width"] = true;
  JsonDocument response;
  DeserializationError error = deserializeJson(response, http.getStream(),
                                               DeserializationOption::Filter(filter));
  http.end();
  if (error) { Serial.printf("Spotify artists JSON error: %s\n", error.c_str()); return false; }
  String names[3], urls[3];
  int count = 0;
  for (JsonVariant item : response["items"].as<JsonArray>()) {
    if (count == 3) break;
    names[count] = item["name"].as<String>();
    for (JsonVariant image : item["images"].as<JsonArray>()) {
      int width = image["width"] | 0;
      if (width > 0 && width <= 320 &&
          (!urls[count].length() || width > 88)) {
        urls[count] = image["url"].as<String>();
        if (width >= 160) break;
      }
    }
    ++count;
  }
  for (int i = 0; i < 3; ++i) {
    if (urls[i] != topImageUrls[i] || !topImages[i]) {
      topImageUrls[i] = urls[i];
      downloadJpeg(urls[i], topImages[i], topImageSizes[i]);
    }
    topNames[i] = names[i];
  }
  topArtistCount = count;
  topArtistStatus = count ? "" : "No recent artists";
  topArtistsFetchedAt = millis();
  Serial.printf("Loaded %d short-term top artists\n", count);
  return true;
}

void updateLyrics() {
  if (!syncedLyrics.length()) return;
  lyric = "";
  nextLyric = "";
  int start = 0;
  while (start < syncedLyrics.length()) {
    int end = syncedLyrics.indexOf('\n', start);
    if (end < 0) end = syncedLyrics.length();
    String line = syncedLyrics.substring(start, end);
    int colon = line.indexOf(':'), close = line.indexOf(']');
    if (line.startsWith("[") && colon > 1 && close > colon) {
      uint32_t at = line.substring(1, colon).toInt() * 60000UL +
                    (uint32_t)(line.substring(colon + 1, close).toFloat() * 1000);
      String words = line.substring(close + 1);
      words.trim();
      bool hadMusicNote = words.indexOf("♩") >= 0 || words.indexOf("♪") >= 0 ||
                          words.indexOf("♫") >= 0 || words.indexOf("♬") >= 0;
      words.replace("♩", ""); words.replace("♪", "");
      words.replace("♫", ""); words.replace("♬", "");
      words.trim();
      if (hadMusicNote && !words.length()) words = "Instrumental";
      if (at <= progressMs) lyric = words;
      else if (!nextLyric.length() && words.length() && words != lyric) nextLyric = words;
    }
    start = end + 1;
  }
}

bool hasUsableLyrics(const String &lines) {
  int start = 0;
  while (start < lines.length()) {
    int end = lines.indexOf('\n', start);
    if (end < 0) end = lines.length();
    String line = lines.substring(start, end);
    int colon = line.indexOf(':'), close = line.indexOf(']');
    if (line.startsWith("[") && colon > 1 && close > colon) {
      line = line.substring(close + 1);
      line.replace("♩", ""); line.replace("♪", "");
      line.replace("♫", ""); line.replace("♬", "");
      line.trim();
      if (line.length() && !line.equalsIgnoreCase("[Music]")) return true;
    }
    start = end + 1;
  }
  return false;
}

void lowerAscii(String &value) {
  for (size_t i = 0; i < value.length(); ++i)
    if (value[i] >= 'A' && value[i] <= 'Z')
      value.setCharAt(i, value[i] + ('a' - 'A'));
}

String normalizedTitle(String value) {
  lowerAscii(value);
  value.replace("(", " "); value.replace(")", " ");
  value.replace("-", " "); value.replace("[", " "); value.replace("]", " ");
  value.replace("–", " "); value.replace("—", " ");
  while (value.indexOf("  ") >= 0) value.replace("  ", " ");
  value.trim();
  return value;
}

bool requestLyrics(const String &name, const String &by, const String &album,
                   uint32_t lengthMs, String &lines, String &statusText) {
  lines = "";
  statusText = "Unavailable";
  Serial.printf("LRCLIB lookup: %s | %s | %s | %lu ms\n",
                name.c_str(), by.c_str(), album.c_str(), (unsigned long)lengthMs);
  String url = "https://lrclib.net/api/get?track_name=" + urlEncode(name) +
               "&artist_name=" + urlEncode(by) + "&album_name=" + urlEncode(album) +
               "&duration=" + String((lengthMs + 500) / 1000);
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "spotify-smart-frame/0.1");
  int responseCode = http.GET();
  if (responseCode == 200) {
    JsonDocument response;
    String body = http.getString(); // HTTPClient removes chunk framing here.
    DeserializationError error = deserializeJson(response, body);
    if (!error) {
      lines = response["syncedLyrics"].as<String>();
      statusText = response["instrumental"] | false ? "Instrumental" : "Unavailable";
      if (lines.length() && !hasUsableLyrics(lines)) {
        lines = "";
        if (response["plainLyrics"].as<String>().length())
          statusText = "Unavailable (not synchronized)";
      }
      Serial.printf("LRCLIB exact id=%d title=%s duration=%.1f synced bytes=%u\n",
                    response["id"] | 0, name.c_str(),
                    response["duration"] | 0.0f, (unsigned)lines.length());
    } else {
      Serial.printf("LRCLIB JSON error: %s\n", error.c_str());
    }
  } else {
    Serial.printf("LRCLIB HTTP %d\n", responseCode);
  }
  http.end();
  bool searchCompleted = false;
  if (!lines.length() && statusText != "Instrumental") {
    int separator = by.indexOf(", ");
    String primaryArtist = by.substring(0, separator < 0 ? by.length() : separator);
    String baseTitle = name;
    int suffix = baseTitle.indexOf(" (");
    int dash = baseTitle.indexOf(" - ");
    if (suffix < 0 || (dash >= 0 && dash < suffix)) suffix = dash;
    if (suffix >= 0) baseTitle = baseTitle.substring(0, suffix);
    String searchUrl = "https://lrclib.net/api/search?track_name=" + urlEncode(baseTitle) +
                       "&artist_name=" + urlEncode(primaryArtist);
    bool nonAsciiTitle = false;
    for (size_t i = 0; i < name.length(); ++i)
      if ((uint8_t)name[i] >= 0x80) { nonAsciiTitle = true; break; }
    if (nonAsciiTitle)
      searchUrl = "https://lrclib.net/api/search?track_name=" + urlEncode(baseTitle);
    delay(250);
    NetworkClientSecure searchClient;
    searchClient.useBuiltinCACertBundle();
    HTTPClient search;
    search.setTimeout(15000);
    if (search.begin(searchClient, searchUrl)) {
      search.addHeader("User-Agent", "spotify-smart-frame/0.1");
      int searchStatus = search.GET();
      if (searchStatus == 200) {
        JsonDocument matches;
        DeserializationError searchError = deserializeJson(matches, search.getString());
        if (!searchError) {
          searchCompleted = true;
          bool bestAlbum = false;
          bool bestFullTitle = false;
          float bestDifference = 4;
          int bestId = 0;
          String uniqueTitleLines;
          int uniqueTitleId = 0;
          bool ambiguousTitle = false;
          lowerAscii(primaryArtist);
          String fullTitle = normalizedTitle(name);
          String shortTitle = normalizedTitle(baseTitle);
          int albumSeparator = album.indexOf(" - ");
          String albumBase = normalizedTitle(album.substring(
              0, albumSeparator < 0 ? album.length() : albumSeparator));
          for (JsonObject item : matches.as<JsonArray>()) {
            String candidateName = item["trackName"].as<String>();
            String candidateArtist = item["artistName"].as<String>();
            String candidateLines = item["syncedLyrics"].as<String>();
            lowerAscii(candidateArtist);
            float difference = fabsf((item["duration"] | 0.0f) - lengthMs / 1000.0f);
            String candidateAlbum = item["albumName"].as<String>();
            int albumSuffix = candidateAlbum.indexOf(" - ");
            String candidateAlbumBase = normalizedTitle(candidateAlbum.substring(
                0, albumSuffix < 0 ? candidateAlbum.length() : albumSuffix));
            bool sameAlbum = candidateAlbum.equalsIgnoreCase(album) ||
                             (albumBase.length() && candidateAlbumBase == albumBase);
            String candidateTitle = normalizedTitle(candidateName);
            bool fullTitleMatch = candidateTitle == fullTitle;
            bool artistMatch = candidateArtist.indexOf(primaryArtist) >= 0;
            if ((!fullTitleMatch && !(sameAlbum && candidateTitle == shortTitle)) ||
                difference > 3 || !hasUsableLyrics(candidateLines)) continue;
            if (!artistMatch && !(nonAsciiTitle && sameAlbum)) {
              // A transliterated artist can still have one exact-title, close-duration match.
              if (nonAsciiTitle && fullTitleMatch) {
                if (uniqueTitleId) ambiguousTitle = true;
                else { uniqueTitleLines = candidateLines; uniqueTitleId = item["id"] | 0; }
              }
              continue;
            }
            if (!lines.length() || (fullTitleMatch && !bestFullTitle) ||
                (fullTitleMatch == bestFullTitle && sameAlbum && !bestAlbum) ||
                (fullTitleMatch == bestFullTitle && sameAlbum == bestAlbum &&
                 difference < bestDifference)) {
              lines = candidateLines;
              bestFullTitle = fullTitleMatch;
              bestAlbum = sameAlbum;
              bestDifference = difference;
              bestId = item["id"] | 0;
            }
          }
          if (!lines.length() && uniqueTitleId && !ambiguousTitle) {
            lines = uniqueTitleLines;
            bestId = uniqueTitleId;
          }
          if (lines.length()) Serial.printf("LRCLIB alternate id=%d synced bytes=%u\n",
                                            bestId, (unsigned)lines.length());
          else Serial.printf("LRCLIB search returned %u records, no close synced match\n",
                             (unsigned)matches.as<JsonArray>().size());
        } else Serial.printf("LRCLIB search JSON error: %s\n", searchError.c_str());
      } else Serial.printf("LRCLIB search HTTP %d\n", searchStatus);
      search.end();
    }
  }
  return lines.length() || statusText == "Instrumental" || searchCompleted;
}

void lookupLyrics(const String &album) {
  lyricAlbum = album;
  syncedLyrics = "";
  lyric = "Loading...";
  nextLyric = "";
  render();
  lyricRetryPending = !requestLyrics(title, artist, album, durationMs, syncedLyrics, lyric);
  lastLyricsAttempt = millis();
  if (lyricRetryPending) {
    lyric = "Searching lyrics...";
    Serial.println("LRCLIB lookup incomplete; will retry this track");
  }
}

bool prefetchNextTrack(const String &token, const String &currentId,
                        String &nextId, String &lines, String &statusText) {
  if (spotifyRateLimited()) return false;
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/queue")) return false;
  const char *headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Authorization", "Bearer " + token);
  int status = http.GET();
  Serial.printf("Spotify request queue #%lu uptime=%lu s HTTP %d\n",
                (unsigned long)++queueRequestCount,
                (unsigned long)(millis() / 1000), status);
  if (status != 200) {
    if (status == 429) noteSpotifyRateLimit(http);
    Serial.printf("Spotify queue HTTP %d\n", status);
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["queue"][0]["type"] = true;
  filter["queue"][0]["id"] = true;
  filter["queue"][0]["name"] = true;
  filter["queue"][0]["artists"][0]["name"] = true;
  filter["queue"][0]["album"]["name"] = true;
  filter["queue"][0]["duration_ms"] = true;
  JsonDocument queue;
  DeserializationError error = deserializeJson(queue, http.getStream(),
                                               DeserializationOption::Filter(filter));
  http.end();
  if (error) { Serial.printf("Spotify queue JSON error: %s\n", error.c_str()); return false; }
  String nextName, nextArtist, nextAlbum;
  uint32_t nextDuration = 0;
  for (JsonVariant item : queue["queue"].as<JsonArray>()) {
    if (item["type"].as<String>() != "track") continue;
    nextId = item["id"].as<String>();
    nextName = item["name"].as<String>();
    nextAlbum = item["album"]["name"].as<String>();
    nextDuration = item["duration_ms"] | 0;
    for (JsonVariant value : item["artists"].as<JsonArray>()) {
      if (nextArtist.length()) nextArtist += ", ";
      nextArtist += value["name"].as<String>();
    }
    break;
  }
  if (!nextId.length() || nextId == currentId) return false;
  queue.clear();
  if (!requestLyrics(nextName, nextArtist, nextAlbum, nextDuration, lines, statusText))
    return false;
  Serial.printf("Prefetched next track lyrics: %u bytes\n", (unsigned)lines.length());
  return true;
}

struct PrefetchRequest {
  String token, currentId;
  uint32_t generation;
};

void prefetchTask(void *argument) {
  PrefetchRequest *request = (PrefetchRequest *)argument;
  String nextId, lines, statusText;
  bool ready = prefetchNextTrack(request->token, request->currentId,
                                 nextId, lines, statusText);
  if (ready && xSemaphoreTake(prefetchMutex, portMAX_DELAY) == pdTRUE) {
    if (request->generation == prefetchGeneration) {
      prefetchedTrackId = nextId;
      prefetchedLyrics = lines;
      prefetchedStatus = statusText;
    }
    xSemaphoreGive(prefetchMutex);
  }
  delete request;
  prefetchBusy = false;
  vTaskDelete(nullptr);
}

bool startPrefetch() {
  if (prefetchBusy || !prefetchMutex) return false;
  PrefetchRequest *request = new PrefetchRequest{accessToken, trackId, prefetchGeneration};
  if (!request) return false;
  prefetchBusy = true;
  if (xTaskCreate(prefetchTask, "queue-lyrics", 16384, request, 1, nullptr) != pdPASS) {
    prefetchBusy = false;
    delete request;
    Serial.println("Could not start queue prefetch task");
    return false;
  }
  return true;
}

bool fetchPlayback() {
  if (spotifyRateLimited()) return false;
  if (!accessToken.length() || (int32_t)(millis() - tokenExpiresAt) >= 0) {
    if (!refreshAccessToken()) return false;
  }
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) return false;
  const char *headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Authorization", "Bearer " + accessToken);
  int status = http.GET();
  Serial.printf("Spotify request playback #%lu uptime=%lu s HTTP %d\n",
                (unsigned long)++playbackRequestCount,
                (unsigned long)(millis() / 1000), status);
  if (status == 204) {
    http.end();
    isPlaying = false;
    if (!inactiveSince) inactiveSince = millis() ? millis() : 1;
    render();
    return true;
  }
  if (status == 401) accessToken = "";
  if (status == 429) noteSpotifyRateLimit(http);
  if (status != 200) { Serial.printf("Spotify playback HTTP %d\n", status); http.end(); return false; }
  JsonDocument state;
  DeserializationError error = deserializeJson(state, http.getStream());
  http.end();
  if (error || state["item"].isNull()) return false;

  playing = true;
  isPlaying = state["is_playing"] | false;
  if (isPlaying) inactiveSince = 0;
  else if (!inactiveSince) inactiveSince = millis() ? millis() : 1;
  title = state["item"]["name"].as<String>();
  artist = "";
  for (JsonVariant value : state["item"]["artists"].as<JsonArray>()) {
    if (artist.length()) artist += ", ";
    artist += value["name"].as<String>();
  }
  progressMs = state["progress_ms"] | 0;
  durationMs = state["item"]["duration_ms"] | 0;
  polledAt = millis();
  JsonArray images = state["item"]["album"]["images"].as<JsonArray>();
  String newArtUrl = images.size() > 1 ? images[1]["url"].as<String>()
                                       : images[0]["url"].as<String>();
  if (newArtUrl != artUrl || (!art && newArtUrl.length())) {
    artUrl = newArtUrl;
    loadArtwork();
  }
  String newTrackId = state["item"]["id"].as<String>();
  if (newTrackId != trackId) {
    trackId = newTrackId;
    lyricRetryPending = false;
    prefetchAttemptedForTrack = "";
    bool usedCache = false;
    if (prefetchMutex && xSemaphoreTake(prefetchMutex, portMAX_DELAY) == pdTRUE) {
      ++prefetchGeneration;
      if (trackId.length() && trackId == prefetchedTrackId &&
          (prefetchedLyrics.length() || prefetchedStatus == "Instrumental")) {
        syncedLyrics = prefetchedLyrics;
        lyric = prefetchedStatus;
        nextLyric = "";
        usedCache = true;
      }
      prefetchedTrackId = ""; prefetchedLyrics = ""; prefetchedStatus = "";
      xSemaphoreGive(prefetchMutex);
    }
    if (usedCache) Serial.println("Using prefetched lyrics");
    else
      lookupLyrics(state["item"]["album"]["name"].as<String>());
  }
  updateLyrics();
  render();
  if (isPlaying && durationMs > progressMs && durationMs - progressMs <= 65000 &&
      prefetchAttemptedForTrack != trackId && startPrefetch())
    prefetchAttemptedForTrack = trackId;
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Boot, reset reason %d\n", (int)esp_reset_reason());
  Serial.printf("Spotify frame firmware built %s %s\n", __DATE__, __TIME__);
  Serial.printf("Spotify poll interval: playing %lu s, idle %lu s\n",
                (unsigned long)(PLAYING_POLL_MS / 1000),
                (unsigned long)(IDLE_POLL_MS / 1000));
  prefetchMutex = xSemaphoreCreateMutex();
  if (!display.begin()) { Serial.println("Display init failed; check OPI PSRAM setting"); return; }
  display.setRotation(ROTATION_90);
  touchReady = touch.begin();
  if (touchReady) display.setTouchRotation(&touch);
  else Serial.println("Touch init failed; tap refresh disabled");
  display.getCanvas()->fillScreen(0x0841);
  drawLine("Connecting to Wi-Fi...", 20, 140, 440, 24, 0xFFFF);
  display.flush();
  preferences.begin("spotify", false);
  refreshToken = preferences.getString("refresh", SPOTIFY_REFRESH_TOKEN);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 10000) {
      Serial.printf("Wi-Fi status %d; retrying\n", WiFi.status());
      WiFi.reconnect();
      lastRetry = millis();
    }
    delay(500);
    return;
  }
  static bool clockConfigured = false;
  if (!clockConfigured) {
    Serial.println("Wi-Fi connected; syncing clock");
    display.getCanvas()->fillScreen(0x0841);
    drawLine("Syncing clock...", 20, 140, 440, 24, 0xFFFF);
    display.flush();
    configTzTime("IST-2IDT,M3.5.5/2,M10.5.0/2", "pool.ntp.org");
    clockConfigured = true;
  }
  if (time(nullptr) < 1700000000) {
    static uint32_t lastClockLog = 0;
    if (millis() - lastClockLog > 10000) { Serial.println("Waiting for network time"); lastClockLog = millis(); }
    delay(500);
    return;
  }
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  uint8_t brightness = (local.tm_hour >= 23 || local.tm_hour < 6) ? NIGHT_BACKLIGHT : 255;
  static int lastBrightness = -1;
  if (brightness != lastBrightness) {
    analogWrite(JC3248_TFT_BL, brightness);
    lastBrightness = brightness;
  }
  static uint32_t nextPollAt = 0, lastDraw = 0;
  TouchPoint point;
  bool touching = touchReady && touch.read(point);
  if (touching && !touchWasDown && (!lastTapAt || millis() - lastTapAt >= 1500)) {
    lastTapAt = millis();
    if (spotifyRateLimited()) {
      tapFeedback = "QUOTA ACTIVE";
      tapFeedbackUntil = millis() + 2500;
    } else {
      tapRefreshPending = true;
      tapFeedback = "CHECKING SPOTIFY...";
      tapFeedbackUntil = millis() + 30000;
    }
    render();
  }
  touchWasDown = touching;
  if (tapRefreshPending && spotifyRateLimited()) {
    tapRefreshPending = false;
    tapFeedback = "QUOTA ACTIVE";
    tapFeedbackUntil = millis() + 2500;
    render();
  }
  if (!spotifyRateLimited() && !prefetchBusy &&
      (tapRefreshPending || !nextPollAt || (int32_t)(millis() - nextPollAt) >= 0)) {
    bool tapped = tapRefreshPending;
    tapRefreshPending = false;
    bool fetched = fetchPlayback();
    if (!fetched && !spotifyRateLimited()) Serial.println("Cannot fetch Spotify playback");
    if (!fetched && spotifyRateLimited() && (!playing || !isPlaying)) render();
    if (tapped) {
      tapFeedback = spotifyRateLimited() ? "QUOTA ACTIVE" :
                    fetched ? "SPOTIFY UPDATED" : "CHECK FAILED";
      tapFeedbackUntil = millis() + 2500;
      render();
    }
    uint32_t pollDelay = fetched ? (isPlaying ? PLAYING_POLL_MS : IDLE_POLL_MS) : 15000UL;
    nextPollAt = millis() + pollDelay;
  }
  if (tapFeedback && (int32_t)(millis() - tapFeedbackUntil) >= 0) {
    tapFeedback = nullptr;
    render();
  }
  if (playing && isPlaying && lyricRetryPending && !prefetchBusy &&
      millis() - lastLyricsAttempt >= 20000) {
    lookupLyrics(lyricAlbum);
    updateLyrics();
    render();
  }
  if ((!playing || !isPlaying) && !prefetchBusy && !spotifyRateLimited() &&
      (!topArtistsFetchedAt || millis() - topArtistsFetchedAt >= 21600000UL) &&
      (!topArtistsAttemptedAt || millis() - topArtistsAttemptedAt >= 60000UL)) {
    topArtistsAttemptedAt = millis();
    if (fetchTopArtists() || spotifyRateLimited()) render();
  }
  uint32_t nowMs = millis();
  if (playing && isPlaying && nowMs - lastDraw >= 500) {
    progressMs = min(durationMs, progressMs + (nowMs - polledAt));
    polledAt = nowMs;
    lastDraw = nowMs;
    updateLyrics();
    render();
  }
  if (!playing || !isPlaying) {
    static int lastIdleMinute = -1;
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_min != lastIdleMinute) { render(); lastIdleMinute = local.tm_min; }
  }
  delay(50);
}
