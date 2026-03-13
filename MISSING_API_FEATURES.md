# VitaPlex - Missing Plex API Features

## Currently Implemented (55+ endpoints)

Authentication, library browsing, hubs/discovery, media playback (direct + transcode), progress/timeline reporting (with playQueueItemID), stream/subtitle selection, image transcoding, full playlist CRUD, server-side play queues (create/shuffle/unshuffle/move/remove/add), Live TV/DVR tuning + EPG, downloads, extras/trailers.

---

## Missing Features

### High Impact

#### 1. User Ratings
- `PUT /:/rate` — Rate an item (set rating value on a ratingKey)

#### 2. Related / Similar / Post-play
- `GET /library/metadata/{id}/related` — Get related items for any metadata item
- `GET /library/metadata/{id}/similar` — Get similar items
- `GET /hubs/metadata/{id}/postplay` — Post-play recommendations ("You might also like")
- `GET /hubs/promoted` — Promoted hubs for home screen

### Medium Impact

#### 3. Library Sorting & Filtering
- `GET /library/sections/{id}/sorts` — Available sort options (title, year, rating, date added)
- `GET /library/sections/{id}/filters` — Available filter fields (year, decade, genre, director)
- `GET /library/sections/{id}/firstCharacters` — Alphabetical jump bar index
- `GET /library/sections/{id}/autocomplete` — Autocomplete for search within a section

#### 4. Person / Actor Browsing
- `GET /library/people/{personId}` — Get person details (bio, photo)
- `GET /library/people/{personId}/media` — Get all media for a person (filmography)

#### 5. Watch History
- `GET /status/sessions/history/all` — View playback history (filter by account, library, date)
- `DELETE /status/sessions/history/{id}` — Delete a history entry

#### 6. BIF Thumbnail Seek Preview
- `GET /library/parts/{partId}/indexes/{index}` — Get BIF index (thumbnail scrubbing)
- `GET /library/parts/{partId}/indexes/{index}/{offset}` — Get single BIF image at offset

#### 7. Chapter Images
- `GET /library/media/{mediaId}/chapterImages/{chapter}` — Get chapter thumbnail for navigation

#### 8. Section Hubs
- `GET /hubs/sections/{sectionId}` — Curated rows per library section (Recently Added, By Genre, Trending)

#### 9. DVR Recording Management
- `GET /media/subscriptions` — View recording subscriptions
- `POST /media/subscriptions` — Create a recording rule
- `DELETE /media/subscriptions/{id}` — Cancel a recording
- `GET /media/subscriptions/scheduled` — View upcoming scheduled recordings

### Lower Impact

#### 10. Collections Management
Can browse collections but cannot create/modify.

- `POST /library/collections` — Create a collection
- `PUT /library/collections/{id}/items` — Add items to a collection
- `PUT /library/collections/{id}/items/{itemId}/move` — Reorder items

#### 11. Real-Time Event Notifications
- `GET /:/websocket/notifications` — WebSocket event stream
- `GET /:/eventsource/notifications` — Server-Sent Events stream

#### 12. Active Sessions (Now Playing)
- `GET /status/sessions` — List all active playback sessions on the server

#### 13. All Episodes View
- `GET /library/metadata/{id}/allLeaves` — Get all episodes in a show (flattened, no season nav)

#### 14. Voice / Fuzzy Search
- `GET /hubs/search/voice` — Voice-optimized search using Levenshtein distance

#### 15. Random Artwork / Screensaver
- `GET /library/randomArtwork` — Get random artwork across sections

#### 16. Transient Token
- `POST /security/token` — Get a temporary delegation token

#### 17. Metadata Refresh
- `PUT /library/metadata/{id}/refresh` — Force metadata refresh for an item
- `POST /library/sections/{id}/refresh` — Refresh an entire section

#### 18. Server Logging
- `PUT /log` — Write client log messages to the PMS log

---

## Notes on Existing Implementation

- Timeline reports now include `playQueueItemID` when playing from a server queue
- Timeline reports don't send the `continuing` flag (server doesn't know about auto-play-next chain)
- No performance metrics in timeline (`timeToFirstFrame`, `bandwidth`, `bufferedTime`)
- Markers (intro/credits) are read-only — no marker creation/editing API used
- Server play queues are used when online; client-side MusicQueue used for offline/downloaded playback
