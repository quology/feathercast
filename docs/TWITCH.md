# Twitch integration in VELA

Enable **View → Twitch channel** and **View → Twitch chat**. Both are native Qt docks: resize, move, tab, or float them independently. Their visibility and placement use the existing saved workspace layout. Connecting to Twitch does not start or configure a broadcast; streaming still uses the broadcast settings.

## One-time setup

1. Register your own application in the [Twitch developer console](https://dev.twitch.tv/console/apps). Twitch requires a verified account and two-factor authentication. Select the **Public** client type for this installed Windows application. If the registration form requires a redirect URL, use `http://localhost:3000` as a placeholder. Device authorization does not call that URL.
2. Copy the application's **Client ID** into the Twitch channel dock. Do not enter a client secret. The Client ID is public and is saved in the local UI preferences.
3. Click **Connect Twitch**, then **Open Twitch authorization**. Complete authorization yourself in the browser using the displayed code. VELA requests `channel:manage:broadcast`, `user:read:chat`, and `user:write:chat` to edit your channel and read/send chat as your account.
4. Once connected, the dock shows the account and polls Twitch every 30 seconds for live/offline state, viewer count, and stream start time. Uptime advances locally between successful polls. A failed status request displays unavailable until a later successful update.

The flow follows Twitch's [public Device Code Grant documentation](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow). Authorization may expire or be declined; click Connect again to start over. Disconnect also cancels a pending authorization.

## Channel and chat

Edit your title and comma-separated tags, search for a category, then explicitly choose a category from the results. Searching preserves the current category; **No category** clears it. Click **Save channel information** to send the changes. Polling preserves unsaved edits. Titles require 1–140 characters. Use up to 10 distinct tags, each 1–25 Unicode letters or numbers, without spaces or punctuation. Limits are checked before sending; Twitch remains authoritative about accepted values. See [Modify Channel Information](https://dev.twitch.tv/docs/api/reference/channels/#modify-channel-information).

The chat dock receives plain-text messages for the connected account's own channel via [EventSub WebSocket](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/), and sends through [Helix Send Chat Message](https://dev.twitch.tv/docs/chat/send-receive-messages/). Press Enter or click Send to send 1–500 characters on one line. Failed sends preserve the draft; Twitch may block or hold a message for moderation. No message is resent automatically after a network failure. Chat history retains at most 500 display lines in memory and clears on disconnect. This is a basic text chat client; it does not render graphical emotes, badges, or moderation controls.

Chat reconnects with bounded backoff after connection loss. Twitch-directed reconnects retain the old socket until the replacement's welcome, preserving the transferred subscription. Normal connection loss requires a fresh subscription; messages during that gap cannot be recovered. Authorization revocation ends the local session. A permanent subscription rejection requires disconnecting/reconnecting rather than retrying indefinitely.

## Credentials and recovery

Access and refresh tokens are stored together in `twitch.tokens` under the existing application profile directory, encrypted with Windows DPAPI for the current Windows user. They are separate from broadcast configuration and UI preferences. Do not share the token file. A profile lock prevents two app instances from rotating the same single-use refresh token. No client secret is stored or required.

The app [validates tokens at startup and hourly](https://dev.twitch.tv/docs/authentication/validate-tokens/), checks the client/account/scopes, and [refreshes on HTTP 401](https://dev.twitch.tv/docs/authentication/refresh-tokens/). Concurrent expired requests share one refresh; the rotated pair is saved atomically before resuming. Permanent authentication failures clear the credentials. Temporary validation/refresh failures end the active session but retain encrypted credentials for a manual Connect retry. A lost refresh response can consume a single-use refresh token, so a later retry may require fresh browser authorization. Public refresh tokens expire after inactivity; reauthorize when prompted.

Requests have transfer and hard deadlines, response-size limits, bounded concurrent requests, and sanitized error messages. Rate limits pause the relevant request bucket; chat sends have a separate bucket from general Helix requests. Rate-limited changes or messages require a deliberate retry after the displayed failure. No raw token responses or server error bodies are shown. See [Twitch rate limits](https://dev.twitch.tv/docs/api/guide/#twitch-rate-limits).

**Disconnect** signs out locally, cancels pending requests/chat, and removes saved tokens. It does not revoke the grant at Twitch. To revoke access fully, remove the application under [Twitch Settings → Connections](https://www.twitch.tv/settings/connections). If removal of the local credential file fails, the app reports that separately; remove the file before restarting.

## Build and validation

`FEATHERCAST_TWITCH=ON` is the default for app builds and requires Qt 6 Network and WebSockets. `-DFEATHERCAST_TWITCH=OFF` omits the service and docks; no encoding or packet-transport dependency is introduced. Qt's Windows Schannel TLS backend uses Windows certificate trust.

`feathercast_twitch_tests` runs local HTTP/WebSocket fixtures for DPAPI roundtrip, device pending/success, startup validation, missing scopes, rotated single-flight refresh, request encoding, live/offline/error status, chat receive/dedup/send, separate rate buckets, profile locking, and stale callbacks after disconnect. `feathercast_twitch_ui_tests` exercises production docks, plain-text bounded history, category selection, dirty edit preservation, live uptime, and floating/resizing. These tests use fake credentials and do not authorize an account or send real Twitch chat. Authenticated end-to-end account authorization and Twitch-directed reconnect handoff still require a manual test with the user's application/account.
