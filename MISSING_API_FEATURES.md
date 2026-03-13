# VitaPlex - Missing Plex API Features

## Currently Implemented (45+ endpoints)

Authentication, library browsing, hubs/discovery, media playback (direct + transcode), progress/timeline reporting, stream/subtitle selection, image transcoding, full playlist CRUD, Live TV/DVR tuning + EPG, downloads.

---

## Missing Features

### High Impact

#### 1. Play Queue Management
Server-side play queue for "Up Next", continuous play, shuffle/repeat. Currently using client-side MusicQueue only.

- `POST /playQueues` — Create a play queue (shuffle, repeat, continuous play, trailers)
- `GET /playQueues/{id}` — Retrieve play queue with windowed view
- `PUT /playQueues/{id}` — Add items to an existing play queue
- `DELETE /playQueues/{id}/items` — Clear play queue
- `DELETE /playQueues/{id}/items/{itemId}` — Remove single item
- `PUT /playQueues/{id}/items/{itemId}/move` — Reorder items
- `PUT /playQueues/{id}/shuffle` — Shuffle the queue
- `PUT /playQueues/{id}/unshuffle` — Restore natural order
- `PUT /playQueues/{id}/reset` — Reset to first item

#### 2. User Ratings
- `PUT /:/rate` — Rate an item (set rating value on a ratingKey)

#### 3. Extras / Trailers
- `GET /library/metadata/{id}/extras` — Get extras (trailers, deleted scenes, featurettes)

#### 4. Related / Similar / Post-play
- `GET /library/metadata/{id}/related` — Get related items for any metadata item
- `GET /library/metadata/{id}/similar` — Get similar items
- `GET /hubs/metadata/{id}/postplay` — Post-play recommendations ("You might also like")
- `GET /hubs/promoted` — Promoted hubs for home screen

### Medium Impact

#### 5. Library Sorting & Filtering
- `GET /library/sections/{id}/sorts` — Available sort options (title, year, rating, date added)
- `GET /library/sections/{id}/filters` — Available filter fields (year, decade, genre, director)
- `GET /library/sections/{id}/firstCharacters` — Alphabetical jump bar index
- `GET /library/sections/{id}/autocomplete` — Autocomplete for search within a section

#### 6. Person / Actor Browsing
- `GET /library/people/{personId}` — Get person details (bio, photo)
- `GET /library/people/{personId}/media` — Get all media for a person (filmography)

#### 7. Watch History
- `GET /status/sessions/history/all` — View playback history (filter by account, library, date)
- `DELETE /status/sessions/history/{id}` — Delete a history entry

#### 8. BIF Thumbnail Seek Preview
- `GET /library/parts/{partId}/indexes/{index}` — Get BIF index (thumbnail scrubbing)
- `GET /library/parts/{partId}/indexes/{index}/{offset}` — Get single BIF image at offset

#### 9. Chapter Images
- `GET /library/media/{mediaId}/chapterImages/{chapter}` — Get chapter thumbnail for navigation

#### 10. Section Hubs
- `GET /hubs/sections/{sectionId}` — Curated rows per library section (Recently Added, By Genre, Trending)

#### 11. DVR Recording Management
- `GET /media/subscriptions` — View recording subscriptions
- `POST /media/subscriptions` — Create a recording rule
- `DELETE /media/subscriptions/{id}` — Cancel a recording
- `GET /media/subscriptions/scheduled` — View upcoming scheduled recordings

### Lower Impact

#### 12. Collections Management
Can browse collections but cannot create/modify.

- `POST /library/collections` — Create a collection
- `PUT /library/collections/{id}/items` — Add items to a collection
- `PUT /library/collections/{id}/items/{itemId}/move` — Reorder items

#### 13. Real-Time Event Notifications
- `GET /:/websocket/notifications` — WebSocket event stream
- `GET /:/eventsource/notifications` — Server-Sent Events stream

#### 14. Active Sessions (Now Playing)
- `GET /status/sessions` — List all active playback sessions on the server

#### 15. All Episodes View
- `GET /library/metadata/{id}/allLeaves` — Get all episodes in a show (flattened, no season nav)

#### 16. Voice / Fuzzy Search
- `GET /hubs/search/voice` — Voice-optimized search using Levenshtein distance

#### 17. Random Artwork / Screensaver
- `GET /library/randomArtwork` — Get random artwork across sections

#### 18. Transient Token
- `POST /security/token` — Get a temporary delegation token

#### 19. Metadata Refresh
- `PUT /library/metadata/{id}/refresh` — Force metadata refresh for an item
- `POST /library/sections/{id}/refresh` — Refresh an entire section

#### 20. Server Logging
- `PUT /log` — Write client log messages to the PMS log

---

## Notes on Existing Implementation

- Timeline reports don't send the `continuing` flag (server doesn't know about auto-play-next chain)
- No performance metrics in timeline (`timeToFirstFrame`, `bandwidth`, `bufferedTime`)
- Markers (intro/credits) are read-only — no marker creation/editing API used
- Music queue is client-side only — server has no awareness of "Up Next" for video playback
