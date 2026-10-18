# NPC chat (experimental)

Talk to soldiers and civilians, call HQ, and command your squad in plain language, typed or spoken.
A language model of your choice plays the NPCs; the game gives it a live situation report and the
mission briefing, and turns its orders into the game's own squad commands.

Dev panel (`Ctrl+\``) -> **NPC Chat**. Single-player only.

## Using it

| Key (default, rebindable) | |
|---|---|
| `Insert` | open the chat box (Enter sends, Esc closes) |
| `End` (hold) | push to talk (needs a speech-to-text provider) |

Who answers depends on how you address them:

* **A squad member by number or name**: "Two, cover me", "Miller, go prone", "Everyone, follow me".
  Orders work only while you lead the squad.
* **HQ**: "Command, we have a visual on two tangos, please advise". HQ answers over the radio,
  using only the briefing and the contacts your squad actually knows about.
* **Whoever is nearby**: "Hey you, how was your day?" goes to the person you are looking at, or
  else the nearest person. That can be a soldier outside your squad or a civilian. Small talk only;
  they cannot be ordered.

* **The whole squad**: "Everyone, get in a line". Usually the senior soldier answers, but a clear
  order to everybody can be answered by the whole squad at once ("Sir, yes sir!"). Every squad mate
  says it in his own voice, all starting together.

**Call signs** (NPC Chat -> *Squad: tone and call signs*): the tab lists your squad (number, name,
role), and you can give each soldier an optional call sign. "Hammer, radio check" then gets "Hammer
here, go ahead, sir." from that soldier. Giving a squad member "Papa Bear" takes that name away
from HQ; HQ is still reachable as "Command" or "HQ". Call signs are saved per squad number.

**Tone**: one slider from *Strict military* through *Professional* and *Relaxed* to *Buddies*.
It adds a short tone paragraph after your system prompt; the prompt itself is never rewritten.

Orders the model can give (all go through the same SQF order commands the radio menu uses, from a
fixed whitelist; nothing the model writes is run as script): follow / regroup, hold, move (to
where you look, to a unit, contact or vehicle, or N metres on a bearing), cover me, watch, engage
a known contact, stance, combat mode (hold fire / fire at will ...), behaviour, formation (line,
column, staggered column, wedge, echelon left/right, vee), speed, get in a vehicle, get out.

The dev panel also has a **Home** tab: every page grouped by purpose, with one-click jumps and the
panel's keyboard shortcuts.

## Providers

| | Options |
|---|---|
| Language model | OpenRouter, OpenAI, Anthropic (Claude), Google Gemini, Ollama (local, no key), any OpenAI-compatible server |
| Speech to text | OpenAI, Groq Whisper (free tier), Google Cloud Speech, any OpenAI-compatible server (e.g. local whisper.cpp) |
| Text to speech | Windows voices (free, offline, no key), OpenAI, Google Cloud TTS, any OpenAI-compatible server (e.g. local Kokoro) |

ElevenLabs works for both speech directions (voices: `text-to-speech`, input: Scribe). Every provider's
address is pre-filled; only Ollama and Custom servers show the address field openly.

**Connected or not.** After **Set** on a key, the panel checks it at once and shows a light per
service: green *CONNECTED* (with what was found: "key accepted, free tier, 300 models"), red *NOT
CONNECTED* with the provider's own reason and the fix, yellow while checking. OpenRouter's model list
is public, so its key is checked separately (`/key`). The chat box in game shows the same light.

**Models and voices load from the provider.** The model list is filterable (`free`, `llama`,
`free claude`: every word must match; `free` also matches zero-priced models). The voice list
(ElevenLabs, Google, Windows, OpenAI's fixed set, Kokoro-style local servers) has a tick per voice
for the squad's pool and a **Test** button that plays a sample sentence in that voice.

Google Cloud speech has a free monthly allowance but needs a Google Cloud project with billing
enabled and the Speech-to-Text / Text-to-Speech APIs turned on. The cheapest start is Windows voices
plus Groq for speech-to-text, or one OpenAI key for all three.

Each squad member keeps one voice from the voice pool, and HQ always uses the first voice. HQ, and
squad mates more than 40 m away, sound like a radio.

## What is sent

To the chosen provider only, with every request: your editable system prompt, the answer format,
the situation report (mission name, time and weather, your position and heading, your squad and its
state, people within 60 m, the enemy contacts your squad knows, nearby vehicles) and the briefing
text, plus the recent conversation. Each part can be switched off in the panel. "Preview the
situation report" shows exactly what would be sent.

## API keys

* Held in memory for the session unless **Remember API keys on this computer** is ticked.
* When remembered, keys are encrypted with Windows DPAPI (only your Windows account on this
  computer can decrypt them) and written to `%APPDATA%\OpenPoseidon\npc-chat\keys.dat`. That is
  never the game folder, the profile folder, or a git repository: the store refuses to write inside
  any git work tree.
* Environment variables work too and are never stored: `OPENAI_API_KEY`, `OPENROUTER_API_KEY`,
  `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`, `GOOGLE_API_KEY`, `GROQ_API_KEY`.
* Keys never appear in logs, the settings file (`npcChat.json` in the profile folder) or error
  messages: anything key-shaped is redacted.
* The repository's pre-commit hook refuses any commit that adds something shaped like an API key.

A rate limit (12 requests per minute by default) protects your bill from a runaway loop.

## Code

`engine/Poseidon/Dev/NpcChat/`: `NpcChatSettings` (settings, provider defaults), `NpcChatSecrets`
(key store), `NpcChatHttp` / `NpcChatProviders` (wire formats), `NpcChatReply` (the JSON reply
contract and order whitelist), `NpcChatGame` (situation report, speakers, order execution),
`NpcChatSapi` (Windows voices), `NpcChat` (worker thread, conversation, push-to-talk, playback),
`NpcChatUi` (dev-panel tab and in-game overlay). Tests: `tests/unit/engine/Poseidon/Dev/NpcChat/`.

## Later additions

* **Orders, obeyed:** the prompt frames the game as a game and the squad carries out the leader's
  orders without arguing. Besides the list above: `fire` (now, or into the air), `engage` on anyone
  in the report (a person from the report is made hostile first, the game's renegade rule), and
  `line_up` (a real line, 3 m apart, facing the player; formation LINE only shapes following). Get in /
  get out go through the same group command as the radio menu's "Get in".
* **Personas:** each soldier has a one-line persona (defaults from his role), editable in the
  collapsed "Personas" node of the squad section.
* **Voices:** per-soldier voices, "Auto-assign male voices", "Assign my voice pool". The game's own
  radio voices are muted while NPC voices speak (option, on by default). When a voice service refuses
  for the session (quota, key) the line falls back to an installed male Windows voice (English first;
  both the classic and the OneCore voice stores are searched); a single refused voice (ElevenLabs
  library voice on a free plan) is skipped on its own.
* **Models:** language, speech-to-text and voice models are listed from the provider (filterable;
  moderation classifiers are hidden). An unavailable speech model is replaced by a working one.
  Thinking models get extra token headroom; a cut-off answer is recovered and reported.
* **Diagnostics:** connection lights, a per-request log line, an opt-in traffic log
  (`npcChat-traffic.log` in the profile folder, keys hidden), a speech-to-text test button.
* **Settings:** export / import to a file (never contains keys); compact chat-only dev panel;
  push-to-talk on Caps Lock by default.
* **Dev panel Home tab:** every page grouped, with one-click jumps and the panel's shortcuts.
