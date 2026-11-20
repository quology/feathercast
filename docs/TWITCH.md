# Twitch integration in VELA

The **View** menu has four resizable docks. Move, tab, or float each one and VELA remembers your layout.

- **Stream Information** connects your Twitch account and edits the broadcast title, category, and tags.
- **Twitch Stats** shows live or offline status, viewer count, uptime, and when Twitch last responded. It updates every 30 seconds and has a Refresh button.
- **Twitch Activity Feed** shows new followers, subscriptions, shared resubscriptions, gift subs, Bits cheers, and incoming raids while connected.
- **Twitch Chat** receives and sends messages for your channel and supports Twitch emotes plus optional BetterTTV and FrankerFaceZ emotes.

Starting a broadcast still uses VELA's broadcast settings.

## One-time setup

1. Register your own application in the [Twitch developer console](https://dev.twitch.tv/console/apps). Twitch requires a verified account and two-factor authentication. Select the **Public** client type for this installed Windows application. If the registration form requires a redirect URL, use `http://localhost:3000` as a placeholder. Device authorization does not call that URL.
2. Copy the application's **Client ID** into **Stream Information**. The Client ID is public and is saved in the local UI preferences.
3. Click **Connect Twitch**, then **Open Twitch authorization**. Complete authorization in the browser using the displayed code. VELA requests channel-editing and chat permissions, plus read permissions for follows (`moderator:read:followers`), subscriptions (`channel:read:subscriptions`), and Bits (`bits:read`) for the Activity Feed.
4. Once connected, your username appears in **Stream Information**. Older connections can choose **Authorize activity feed** to add the missing permissions. VELA opens Stream Information for the code and retains the previous saved credentials if authorization fails.

The flow follows Twitch's [public Device Code Grant documentation](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow). Authorization may expire or be declined; click Connect again to start over. Disconnect also cancels a pending authorization.

## Stream Information

Edit your title and comma-separated tags, search for a category, then explicitly choose a category from the results. Searching preserves the current category; **No category** clears it. Click **Save channel information** to send the changes. Polling preserves unsaved edits. Titles require 1–140 characters. Use up to 10 distinct tags, each 1–25 Unicode letters or numbers, without spaces or punctuation. Limits are checked before sending; Twitch remains authoritative about accepted values. See [Modify Channel Information](https://dev.twitch.tv/docs/api/reference/channels/#modify-channel-information).

## Activity Feed

The feed shows connection and subscription status and retains the latest 200 events in memory. **Clear** removes the displayed history. Incoming raids are available with an existing VELA connection. Other activity types require the additional permissions listed above, and Twitch controls which events each channel can generate. The feed covers activity received during this connection. [Twitch EventSub channel events](https://dev.twitch.tv/docs/eventsub/reference/channel)

## Chat and emotes

The chat dock receives and sends messages for the connected account's channel. Press Enter or click Send to send 1–500 characters on one line. Failed sends preserve the draft. Twitch may block or hold a message for moderation. A failed message can be sent again manually. Chat history keeps up to 500 messages in memory and clears on disconnect. [Twitch chat documentation](https://dev.twitch.tv/docs/chat/send-receive-messages/)

The **Chat add-ons** selector enables BetterTTV, FrankerFaceZ, or both. VELA fetches global and channel emotes from the providers' public APIs and uses the channel's emotes first when names overlap. Twitch emotes are available with any setting. Copying a message returns its original emote names. GIF and WebP emotes can animate, with a limited number of simultaneous animations. Unavailable artwork appears as the emote name. Provider services receive anonymous emote requests and the public channel ID for catalog lookups. [BetterTTV API](https://betterttv.com/developers) · [FrankerFaceZ API](https://www.frankerfacez.com/developers)

Chat and activity reconnect with bounded backoff after connection loss. Twitch-directed reconnects retain the old socket until the replacement's welcome, preserving active subscriptions. Normal connection loss requires fresh subscriptions. Messages and activity during the gap are missed. Authorization revocation ends the local session. A temporary subscription error is retried independently. A permanent rejection appears in the affected dock while other subscriptions continue. [EventSub WebSocket documentation](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/)

## Credentials and recovery

Access and refresh tokens are stored together in `twitch.tokens` under the existing application profile directory, encrypted with Windows DPAPI for the current Windows user. They are separate from broadcast configuration and UI preferences. Do not share the token file. A profile lock prevents two app instances from rotating the same single-use refresh token. No client secret is stored or required.

The app [validates tokens at startup and hourly](https://dev.twitch.tv/docs/authentication/validate-tokens/), checks the client/account/scopes, and [refreshes on HTTP 401](https://dev.twitch.tv/docs/authentication/refresh-tokens/). Concurrent expired requests share one refresh; the rotated pair is saved atomically before resuming. Permanent authentication failures clear the credentials. Temporary validation/refresh failures end the active session but retain encrypted credentials for a manual Connect retry. A lost refresh response can consume a single-use refresh token, so a later retry may require fresh browser authorization. Public refresh tokens expire after inactivity; reauthorize when prompted.

Requests have transfer and hard deadlines, response-size limits, bounded concurrent requests, and sanitized error messages. Rate limits pause the relevant request bucket; chat sends have a separate bucket from general Helix requests. Rate-limited changes or messages require a deliberate retry after the displayed failure. No raw token responses or server error bodies are shown. See [Twitch rate limits](https://dev.twitch.tv/docs/api/guide/#twitch-rate-limits).

**Disconnect** signs out locally, cancels pending requests/chat, and removes saved tokens. It does not revoke the grant at Twitch. To revoke access fully, remove the application under [Twitch Settings → Connections](https://www.twitch.tv/settings/connections). If removal of the local credential file fails, the app reports that separately; remove the file before restarting.

## Build and validation

`FEATHERCAST_TWITCH=ON` is the default for app builds and requires Qt 6 Network and WebSockets. `-DFEATHERCAST_TWITCH=OFF` omits the service and docks. Qt's Windows Schannel TLS backend uses Windows certificate trust. The packaged app includes Qt's GIF and WebP image plug-ins for chat emotes.

`feathercast_twitch_tests` runs local HTTP/WebSocket fixtures for OAuth and DPAPI, live/offline status, chat, activity events, independent subscription failures, and preserving credentials during a permission upgrade. `feathercast_twitch_ui_tests` exercises the four production docks, bounded chat and activity history, category selection, dirty edit preservation, live and offline stats, and floating and resizing. `feathercast_twitch_emote_tests` exercises provider setup, image rendering, animation, plain-text copy, and isolation of anonymous requests. The fixtures use synthetic credentials. Authenticated end-to-end account, chat, and activity checks require the user's Twitch application and account.
