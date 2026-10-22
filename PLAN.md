# Spotify Smart Frame - Full Project Plan

> A modular, standalone smart display for Spotify that shows album artwork, synchronized lyrics, and contextual music information, with future support for NFC-enabled vinyl records.

This is the original product vision and longer-term roadmap. Some phases and technology questions below describe earlier planning, not the current implementation. For the working ESP32 setup and optional browser demo, see [README.md](README.md).

---

# Project Vision

The goal of this project is not simply to build another Spotify display.

The goal is to build a polished piece of home decor that feels like a commercial product while remaining fully DIY, inexpensive, and highly customizable.

The display should eventually function as a "living music frame" that automatically updates based on what is currently playing on Spotify or based on an NFC-tagged vinyl record.

The project will intentionally follow good software engineering practices rather than becoming a collection of hacked-together scripts.

---

# Core Principles

## 1. Build the cheapest possible MVP

Avoid spending money before validating assumptions.

Every hardware purchase should unlock the next milestone instead of gambling on the entire project working.

AliExpress-compatible hardware is preferred.

## 2. Agile Development

Each iteration should produce something usable.

Instead of building everything at once:

```text
Idea -> Prototype -> Validate -> Improve -> Repeat
```

Every phase should answer a specific technical question before continuing.

## 3. Modular Architecture

Every component should be replaceable.

```text
Spotify -> Playback Module -> UI State -> Display Driver
```

This allows replacing the LCD, eInk display, LED matrix, Raspberry Pi, or ESP32 without rewriting the application.

## 4. Separation of Concerns

Avoid mixing responsibilities. Candidate modules include:

- Spotify Client
- Lyrics Provider
- Image Downloader
- Image Cache
- Display Renderer
- NFC Manager
- Configuration
- Authentication

Each module should have one clear responsibility.

## 5. Hardware Independence

The project should not depend on a specific display. The UI layer should expose something like:

```text
render(state)
```

Whether the output goes to a browser, LCD, LED matrix, or eInk display should only affect the rendering implementation.

---

# Initial Feature Set

The MVP should support:

- Current Spotify track
- Album artwork
- Song title
- Artist
- Playback progress
- Synchronized lyrics
- Automatic updates
- WiFi configuration
- Standalone operation

---

# Future Features

## NFC Vinyl Mode

Each vinyl record receives an NFC sticker. Touching the record to the reader displays:

- Album artwork
- Artist
- Track list
- Release year

Later versions may also start Spotify playback, queue albums, or open artist information.

## Ambient Mode

When Spotify is idle, show a clock, weather, calendar, or favorite album artwork slideshow.

## Smart Home Integration

Potential future integrations:

- Home Assistant
- MQTT
- Spotify Connect
- Last.fm
- Philips Hue
- Audio visualizer

---

# Conceptual Architecture From the Original Plan

```text
WiFi
  |
Spotify Web API
  |
Spotify Client
  |
Playback State
  |-- Lyrics Provider
  |-- Album Artwork
  |
UI State Model
  |
Display Renderer
  |
LCD / eInk / Matrix
```

Everything revolves around a single application state. The renderer should know nothing about Spotify, and Spotify should know nothing about displays.

---

# Technology Decisions

## Controller

### Current Choice: ESP32-S3

Reasons:

- Extremely inexpensive
- Built-in WiFi
- Low power
- Large community
- Plenty of libraries
- Capable of standalone operation
- Does not require an always-running PC

Recommended variant:

- ESP32-S3
- 8 MB PSRAM
- 16 MB flash
- USB-C

### Alternative: Raspberry Pi

Pros:

- Easier software development
- Linux
- More RAM
- Better image processing

Cons:

- Higher cost
- Higher power consumption
- Overkill for the current scope

Decision: only reconsider if the ESP32 proves incapable of the required functionality.

---

# Display Options

## Option A - IPS LCD (Current Favorite)

Pros:

- Fast refresh
- Color
- Excellent for lyrics
- Album artwork looks good
- Cheap

Cons:

- Higher power consumption

Current recommendation: strongest option.

## Option B - eInk

Pros:

- Beautiful aesthetic
- Extremely low power
- Looks like printed artwork

Cons:

- Slow refresh
- Poor for synchronized lyrics
- Ghosting

Recommendation: future version.

## Option C - RGB Matrix

Pros:

- Retro aesthetic
- Fun animations

Cons:

- Poor resolution
- Lyrics are difficult to display

Recommendation: a different project direction.

---

# Lyrics

Spotify does not expose lyrics through its public Web API.

Current flow:

```text
Spotify -> Current Track -> LRCLIB -> Timestamped Lyrics -> Renderer
```

Fallback order:

1. Synchronized lyrics
2. Plain lyrics
3. Album-only mode

---

# Authentication

Spotify OAuth will be required.

Planned flow:

1. Authenticate once from a laptop.
2. Store the refresh token on the ESP32.
3. Refresh automatically.

The device should continue operating without a computer after setup.

---

# Development Philosophy

The project should prioritize correctness over speed.

Every feature should be tested independently, modular, documented, and reusable.

The project should avoid "just making it work." Instead, every component should be written as though it were part of a real product.

---

# Project Roadmap

## Phase 0 - Software Validation

Cost: $0

Goals:

- Spotify authentication
- Detect the playing song
- Retrieve playback state
- Retrieve the album artwork URL
- Detect song changes

Deliverable: a desktop application that reliably tracks Spotify playback.

## Phase 1 - Lyrics Validation

Cost: $0

Goals:

- Integrate LRCLIB
- Retrieve synchronized lyrics
- Match tracks accurately
- Cache lyrics

Deliverable: a desktop application showing synchronized lyrics.

## Phase 2 - UI Prototype

Cost: $0

Goals:

- Build the final interface
- Album artwork
- Song information
- Progress bar
- Lyrics
- Responsive layout

Deliverable: a browser-based prototype. No hardware yet.

## Phase 3 - ESP32 Foundation

Purchase: ESP32-S3

Goals:

- Connect to WiFi
- Make HTTPS requests
- Parse JSON
- Use persistent storage
- Handle Spotify authentication
- Refresh tokens

Deliverable: ESP32 printing live Spotify information over Serial. A laptop is no longer required after flashing.

## Phase 4 - Lyrics on ESP32

Goals:

- Query LRCLIB
- Parse LRC timestamps
- Synchronize using playback position
- Cache lyrics locally

Deliverable: ESP32 capable of determining the current lyric line.

## Phase 5 - Display

Purchase: LCD

Display:

- Song
- Artist
- Progress
- Current lyric

Deliverable: first standalone Spotify frame.

## Phase 6 - Album Artwork

Goals:

- Download the cover
- Decode JPEG
- Resize
- Cache
- Render

Deliverable: complete Spotify display.

## Phase 7 - NFC

Purchase:

- PN532
- NFC stickers

Flow:

```text
Tap vinyl -> Recognize album -> Display artwork -> Optionally launch playback
```

## Phase 8 - Productization

Purchase:

- Frame
- Better power supply
- Cable management

Goals:

- Permanent enclosure
- Clean wiring
- Wall-ready display

---

# Estimated Budget

| Stage | Cost |
| --- | ---: |
| Software prototype | $0 |
| ESP32-S3 | $6-12 |
| LCD display | $10-25 |
| Wiring / power | $3-8 |
| NFC reader and tags | $6-15 |
| Frame | $5-20 |

Expected total: **about $30-80**, depending on hardware choices.

---

# Current Open Questions

## Hardware

- Is the ESP32 capable of handling image decoding and lyric rendering comfortably?
- Which LCD offers the best balance between price and quality?
- Should touch support be included?

## Software

- Which graphics library provides the best performance?
- What is the best image caching strategy?
- How much local storage should be allocated for lyrics?

## UX

Should the display primarily show album artwork or lyrics?

Possible solution: automatic mode switching.

## Spotify

- How should re-authentication be handled if refresh tokens expire?
- Can setup be simplified through a temporary configuration webpage hosted on the ESP32?

---

# Long-Term Goal

The finished project should feel less like a DIY electronics experiment and more like a polished consumer product.

Someone seeing it on a shelf should think:

> "Where did you buy that?"

rather than:

> "You built that?"
