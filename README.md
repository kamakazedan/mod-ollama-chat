<p align="center">
  <img src="./icon.png" alt="Ollama Chat Module" title="Ollama Chat Module Icon">
</p>


# AzerothCore + Playerbots Module: mod-ollama-chat


> [!CAUTION]
> **LLM/AI Disclaimer:** Large Language Models (LLMs) such as those used by this module do not possess intelligence, reasoning, or true understanding. They generate text by predicting the most likely next word based on patterns in their training data—matching vectors, not thinking or comprehension. The quality and relevance of responses depend entirely on the model you use, its training data, and its configuration. Results may vary, and sometimes the output may be irrelevant, nonsensical, or simply not work as expected. This is a fundamental limitation of current AI and LLM technology. Use with realistic expectations.
>
> This module is also in development and can bog down your server due to the nature of running local LLM. Please proceed with this in mind.

> [!IMPORTANT]
> To fully disable Playerbots normal chatter and random chatter that might interfere with this module, set the following settings in your `playerbots.conf`:
> - `AiPlayerbot.EnableBroadcasts = 0` (disables loot/quest/kill broadcasts)
> - `AiPlayerbot.RandomBotTalk = 0` (disables random talking in say/yell/general channels)
> - `AiPlayerbot.RandomBotEmote = 0` (disables random emoting)
> - `AiPlayerbot.RandomBotSuggestDungeons = 0` (disables dungeon suggestions)
> - `AiPlayerbot.EnableGreet = 0` (disables greeting when invited)
> - `AiPlayerbot.GuildFeedback = 0` (disables guild event chatting)
> - `AiPlayerbot.RandomBotSayWithoutMaster = 0` (disables bots talking without a master)

## Overview

***mod-ollama-chat*** is an AzerothCore module that enhances the Player Bots module by integrating external language model (LLM) support via the Ollama API. This module enables player bots to generate dynamic, in-character chat responses using advanced natural language processing locally on your computer (or remotely hosted). Bots are enriched with personality traits, random chatter triggers, and context-aware replies that mimic the language and lore of World of Warcraft.

## Features

- **Ollama LLM Integration:**  
  Bots generate chat responses by querying an external Ollama API endpoint. This enables natural and contextually appropriate in-game dialogue.

- **Other LLM Providers:**  
  Not limited to Ollama. Set `OllamaChat.Provider` to `openai` for anything that speaks the OpenAI Chat Completions format (OpenAI, OpenRouter, Groq, Mistral, DeepSeek, xAI, Gemini, LM Studio, vLLM, llama.cpp) or to `anthropic` for Claude. See [Using Other LLM Providers](#using-other-llm-providers-openai-openrouter-claude).

- **Player Bot Personalities:**  
  When enabled, each bot is assigned a personality type (e.g., Gamer, Roleplayer, Trickster) that modifies its chat style. Personalities influence prompt generation and result in varied, immersive responses.

- **Context-Aware Prompt Generation:**  
  The module gathers extensive context about both the bot and the interacting player—including class, race, role, faction, guild, and more—to generate prompts for the LLM. A comprehensive WoW cheat sheet is appended to every prompt to ensure the LLM replies with accurate lore, terminology, and in-character language spanning Vanilla WoW, The Burning Crusade, and Wrath of the Lich King.

- **Random Chatter:**  
  Bots can periodically initiate random, environment-based chat when a real player is nearby. This feature adds an extra layer of immersion to the game world.

- **Chat Memory (Conversation History):**  
  Bots now have configurable short-term chat memory. Recent conversations between each player and bot are stored and included as context in every LLM prompt, giving responses better context and continuity.

  Bots now recall your recent interactions—responses will reflect the last several lines of chat with each player.

- **Blacklist for Playerbot Commands:**  
  A configurable blacklist prevents bots from responding to chat messages that start with common playerbot command prefixes, ensuring that administrative commands are not inadvertently processed. Additional commands can be appended via the configuration.

- **Asynchronous Response Handling:**  
  Chat responses are generated on separate threads to avoid blocking the main server loop, ensuring smooth server performance.

- **Live Configuration & Personality Reload:**  
  Reload the module’s config and personality packs in-game or from the server console, without restarting.

- **Event-Based Chatter:**  
  Player bots now comment on key in-game events such as quest completion, rare loot, deaths, PvP kills, leveling up, duels, learning spells, and achievements. Remarks are context-aware, immersive, and personality-driven, making the world feel much more alive.

- **Party-Only Bot Responses:**  
  When enabled, bots will only respond to real player messages and events when they are in the same non-raid party. This helps reduce chat spam while maintaining full bot-to-bot communication within parties for immersive group interactions.

- **Think Mode Support:**  
  Bots can leverage LLM models that have reasoning/think modes. Enable internal reasoning for models that support it by setting `OllamaChat.ThinkModeEnableForModule = 1` in **mod-ollama-chat.conf**. When enabled, the API request includes the `think` flag and the bot omits all `thinking` responses from its final reply.

- **Works with mod-playerbots-plus:**  
  With [mod-playerbots-plus](https://github.com/kamakazedan/mod-playerbots-plus) in the same build, bots also trade on the auction house, hang out in the cities, get unstuck, get back up and use raid marks, and talk about it. See [With mod-playerbots-plus](#with-mod-playerbots-plus).

- **Live Reload for Personalities and Settings:**  
  Instantly reload all mod-ollama-chat configuration and personality packs in-game using the `.ollama reload` command with a GM level account or use `ollama reload` from the server console. No server restart required—updates to `.conf` or personality packs (`.sql` files) are applied immediately.

## Installation

> [!IMPORTANT]
> **Cross-Platform Support**: This module now uses cpp-httplib (header-only) instead of curl, eliminating compilation issues on Windows and simplifying installation on all platforms.

1. **Prerequisites:**
   - Ensure you have liyunfan1223's AzerothCore (https://github.com/liyunfan1223/azerothcore-wotlk) installation with the Player Bots (https://github.com/liyunfan1223/mod-playerbots) module enabled.
   - The module depends on:
     - **fmtlib** (https://github.com/fmtlib/fmt) - For string formatting
     - **nlohmann/json** (https://github.com/nlohmann/json) - For JSON processing (**bundled with module** - no installation needed)
     - cpp-httplib (https://github.com/yhirose/cpp-httplib) - Header-only HTTP library (included, no installation needed)
     - Ollama LLM support – set up a local instance of the Ollama API server with the model of your choice. More details at https://ollama.com

2. **Install Dependencies:**

   ### Windows (vcpkg):
   ```bash
   vcpkg install fmt
   ```

   ### Ubuntu/Debian:
   ```bash
   sudo apt update
   sudo apt install libfmt-dev
   ```

   ### CentOS/RHEL/Fedora:
   ```bash
   sudo yum install fmt-devel  # or dnf install fmt-devel
   ```

   ### macOS (Homebrew):
   ```bash
   brew install fmt
   ```

   ### Arch Linux:
   ```bash
   sudo pacman -S fmt
   ```

3. **Clone the Module:**
   ```bash
   cd /path/to/azerothcore/modules
   git clone https://github.com/DustinHendrickson/mod-ollama-chat.git
   ```

4. **Recompile AzerothCore:**
   ```bash
   cd /path/to/azerothcore
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   ```

5. **Configuration:**
   Copy the default configuration file to your server configuration directory and change to match your setup (if not already done):
   ```bash
   cp /path/to/azerothcore/modules/mod-ollama-chat/conf/mod_ollama_chat.conf.dist /path/to/azerothcore/env/dist/etc/modules/mod_ollama_chat.conf
   ```

6. **Restart the Server:**
   ```bash
   ./worldserver
   ```

## Setting up Ollama Server

This module requires a running Ollama server to function. Ollama allows you to run large language models locally on your machine.

### Installing Ollama

Download and install Ollama from [ollama.com](https://ollama.com). It supports Windows, macOS, and Linux.

- **Windows/macOS:** Download the installer from the website and run it.
- **Linux:** Follow the installation instructions for your distribution (e.g., `curl -fsSL https://ollama.com/install.sh | sh`).

### Starting the Ollama Server

Once installed, start the Ollama server:

```bash
ollama serve
```

This will start the server on `http://localhost:11434` by default.

### Running Ollama Across the Network

If you want to run the Ollama server on a different computer than your AzerothCore server, set the `OLLAMA_HOST` environment variable to `0.0.0.0` before starting the server:

```bash
export OLLAMA_HOST=0.0.0.0
ollama serve
```

This binds the server to all network interfaces, allowing connections from other machines on your network. Update the `OllamaChat.Url` in `mod-ollama-chat.conf` to use the IP address of the machine running Ollama (e.g., `http://192.168.1.100:11434`).

> [!WARNING]
> Exposing Ollama to the network may pose security risks. Ensure your firewall allows traffic on port 11434 only from trusted networks, and consider additional security measures if exposing to the internet.

### Pulling a Model

Before using the module, pull a model that the bots will use for generating responses. For example, to pull the Llama 3.2 1B model:

```bash
ollama pull llama3.2:1b
```

You can find available models at [ollama.com/library](https://ollama.com/library). Choose a model that fits your hardware capabilities.

### Connecting the Module

The module connects to the Ollama API via the configuration in `mod-ollama-chat.conf`. The default endpoint is `http://localhost:11434`. If your Ollama server is running on a different host or port, update the `OllamaChat.Url` setting.

### Checking if Ollama is Running

To verify that the Ollama server is running and accessible, you can test the API:

```bash
curl http://localhost:11434/api/tags
```

This should return a JSON response listing available models. If you get a connection error, ensure the server is started and the endpoint is correct.

## Using Other LLM Providers (OpenAI, OpenRouter, Claude)

Ollama is the default, but it is not required. The only Ollama-specific part
of the module is how a single request is encoded, so any service that speaks
one of two common API formats can be used instead. Four settings in
`mod-ollama-chat.conf` control this:

| Setting | Meaning |
|---|---|
| `OllamaChat.Provider` | `ollama` (default), `openai`, or `anthropic` |
| `OllamaChat.Url` | The full URL of the provider's generation endpoint |
| `OllamaChat.Model` | The model name as the provider publishes it |
| `OllamaChat.ApiKey` | The provider's API key. Leave empty for local servers that need none |
| `OllamaChat.ApiKeyHeader` | Optional. Only for services with their own header name, such as Azure OpenAI (`api-key`) |

`openai` means the **OpenAI Chat Completions format**, which is the de facto
standard and is spoken by far more than OpenAI: OpenRouter, Groq, Together,
Mistral, DeepSeek, xAI, Google Gemini (through its OpenAI-compatible
endpoint), LM Studio, vLLM, llama.cpp server, and Ollama's own
`/v1/chat/completions` route all accept it. `anthropic` is the native Claude
Messages API. Claude is also reachable through OpenRouter using `openai`.

### Examples

**OpenAI**

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = https://api.openai.com/v1/chat/completions
OllamaChat.Model    = gpt-4o-mini
OllamaChat.ApiKey   = sk-...
```

**OpenRouter** (one key, access to many models from many vendors)

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = https://openrouter.ai/api/v1/chat/completions
OllamaChat.Model    = openai/gpt-4o-mini
OllamaChat.ApiKey   = sk-or-...
```

**Anthropic (Claude)**

```ini
OllamaChat.Provider = anthropic
OllamaChat.Url      = https://api.anthropic.com/v1/messages
OllamaChat.Model    = claude-haiku-4-5-20251001
OllamaChat.ApiKey   = sk-ant-...
```

**LM Studio** (local, no key needed)

```ini
OllamaChat.Provider = openai
OllamaChat.Url      = http://localhost:1234/v1/chat/completions
OllamaChat.Model    = <the model id LM Studio shows in its server tab>
```

### Steps

1. Edit the four settings above in `mod-ollama-chat.conf`.
2. Restart `worldserver`, or run `.ollama reload` in game or `ollama reload`
   on the console.
3. Run `.ollama status chat`. The first line now shows the provider and whether
   a key is set (the key itself is never printed).
4. Run `.ollama test hello` to send one request and see the reply, or the
   provider's error message if something is wrong (bad key, wrong model name,
   wrong URL).

### What to know

- **HTTPS requires an OpenSSL build.** The CMake output says
  `[mod-ollama-chat] OpenSSL found - HTTPS support enabled` when it is, and
  `.ollama status chat` shows `HTTPS: available`. Without it, an `https://` URL is
  reported as an error at startup and every request fails. On Windows install
  OpenSSL (for example `vcpkg install openssl` or the Win64 OpenSSL installer)
  and re-run CMake; on Linux install `libssl-dev` or `openssl-devel`.
- **Certificates are verified when a key is set.** By default the module skips
  TLS certificate checks (so self-signed reverse proxies in front of a local
  Ollama keep working) but turns them on whenever `OllamaChat.ApiKey` is
  non-empty, since a key should never travel over an unverified connection.
  `OllamaChat.VerifyCertificates` overrides this either way.
- **API keys.** Every hosted provider needs one; put it in `OllamaChat.ApiKey`.
  The module sends it as a bearer token for OpenAI format and as
  `x-api-key` for Anthropic; `OllamaChat.ApiKeyHeader` overrides the header
  name for services like Azure OpenAI. A rejected key (HTTP 401/403) is
  reported once in the log with what to check.
- **Cost and rate limits.** Random chatter and event chatter across many bots
  adds up fast on a metered API, and hosted providers answer bursts with
  HTTP 429. The module waits and retries a bounded number of times
  (`OllamaChat.RateLimitRetries`, honouring the provider's `Retry-After`,
  capped by `OllamaChat.RateLimitMaxWaitSeconds`) and logs the first
  occurrence. Start with `OllamaChat.EnableRandomChatter = 0` and a low
  `OllamaChat.MaxConcurrentQueries`, and watch the provider's usage page before
  opening it up.
- **Settings that do not apply are not sent.** `NumCtx`, `NumThreads`,
  `RepeatPenalty`, `MinP` and `TopK` have no equivalent in the OpenAI format;
  Anthropic additionally has no `TopP`, `Seed`, or penalties. `NumPredict`
  works everywhere (Anthropic requires a cap, so `0` becomes 1024 there).
- **Think mode.** There is no capability probe for these providers, since
  that would be a billed request. Under `ThinkMode = auto` reasoning is never
  requested. Under `on` it is requested on every call (`reasoning_effort` for
  OpenAI format, an extended-thinking budget for Anthropic) and switched off
  for the session if the provider rejects it.
- **Self-healing parameters.** Newer OpenAI models reject `max_tokens` in
  favour of `max_completion_tokens`, and OpenAI reasoning models reject
  explicit sampling values. Both are detected from the first rejection, logged
  once, and the request is retried immediately, so no reply is lost.
- **Keep the key private.** Treat `mod-ollama-chat.conf` like
  `worldserver.conf`: readable only by the account running the server, and
  never committed to a repository.

## Configuration Options

> For a complete list of all available configuration options with comments and defaults, see `mod-ollama-chat.conf.dist` included in this repository.

## Text Commands

The module provides several in-game text commands for administrators (Game Masters) to manage and monitor the Ollama chat functionality. All commands require **SEC_ADMINISTRATOR** security level (GM level 3 or higher).

### `.ollama reload`
Reloads the module's configuration from `mod-ollama-chat.conf` without restarting the server. Also reloads personality packs and sentiment data.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama reload`
- **Console Equivalent:** `ollama reload`

### `.ollama sentiment view [bot_name] [player_name]`
Displays sentiment tracking data between bots and players.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:**
  - `.ollama sentiment view` - Shows all sentiment data
  - `.ollama sentiment view BotName` - Shows sentiment data for a specific bot
  - `.ollama sentiment view BotName PlayerName` - Shows sentiment between specific bot and player
- **Console Equivalent:** `ollama sentiment view [bot] [player]`

### `.ollama sentiment set <bot_name> <player_name> <value>`
Manually sets the sentiment value between a bot and player (0.0 to 1.0).
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama sentiment set BotName PlayerName 0.8`
- **Console Equivalent:** `ollama sentiment set <bot> <player> <value>`

### `.ollama sentiment reset [bot_name] [player_name]`
Resets sentiment data to default values.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:**
  - `.ollama sentiment reset` - Resets all sentiment data
  - `.ollama sentiment reset BotName` - Resets all sentiment data for a specific bot
  - `.ollama sentiment reset BotName PlayerName` - Resets sentiment between specific bot and player
- **Console Equivalent:** `ollama sentiment reset [bot] [player]`

### `.ollama personality get <bot_name>`
Displays the current personality assigned to a bot.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality get BotName`
- **Console Equivalent:** `ollama personality get <bot>`

### `.ollama personality set <bot_name> <personality>`
Manually assigns a personality to a bot.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality set BotName Gamer`
- **Console Equivalent:** `ollama personality set <bot> <personality>`

### `.ollama personality list`
Lists all available personalities and their descriptions.
- **Security Level:** SEC_ADMINISTRATOR
- **Usage:** `.ollama personality list`
- **Console Equivalent:** `ollama personality list`

> [!NOTE]
> All commands can also be executed from the server console by replacing the leading dot (.) with the command prefix used in your console (typically none or a custom prefix).

### `.ollama status [chat|talk|city|party|bots]`

On its own, a few lines: what the module talks to, whether it is keeping up
(requests waiting, written, failed), what was held back in the last minute,
what is going on right now, and, in orange, anything that looks wrong --
requests failing lately, requests piling up faster than the model writes
them, an `https://` URL in a build without HTTPS. This is the first thing to
run when bots go quiet.

With a part, the counts behind it:

- `chat` -- the requests on their way: endpoint, model, key set or not,
  HTTPS, think mode, the queue, what was dropped and blocked and why, crowds,
  conversations, stock phrases, echoes, the last error.
- `talk` -- what goes into the prompts: congratulations, asks, links, gear
  talk, knowledge, sides, chat itself, roleplay, topic weights.
- `city` -- cities, presence and mingling.
- `party` -- groups and dungeons: party instructions, quest progress, gear,
  deaths, loot rolls, potions.
- `bots` -- what bots do for people: food and water, buffs, orders, bags,
  group quests, city life, things given away.

### `.ollama report`

Writes every count, part by part, to `OllamaChatReport.log` in the server's
logs folder (`OllamaChat.Report.File`), and says where. The module also
writes it every `OllamaChat.Report.Minutes` (10), so the latest is there to
read, or send on, when something went wrong and nobody was in game.
`.ollama status all` does the same.

### `.ollama test <prompt>`

Sends one prompt straight to Ollama and writes the raw output and the
post-processed output side by side to the server log (`module.ollamachat`),
along with the round-trip time and whether think mode was used. Turns "the bots
aren't talking" into a one-command diagnosis.

## How It Works

1. **Chat Filtering and Triggering**  
   When a player (or bot) sends a chat message, the module checks the message's type, distance, and if it starts with any configured blacklist command prefix. If party restrictions are enabled, only bots in the same non-raid party as the real player can respond. Only eligible messages in range and not matching the blacklist will trigger a bot response.

2. **Bot Selection**  
   The system gathers all bots within the relevant distance, determines eligibility based on player/bot reply chance, and caps responses per message using `MaxBotsToPick` and related settings.

3. **Prompt Assembly**  
   For each reply, a prompt is assembled by combining configurable templates with live in-game context: bot/player class, race, gender, role/spec, faction, guild, level, zone, gold, group, environment info, personality, and if enabled, recent chat history between that player and the bot.

4. **LLM Request**  
   The prompt is sent to the Ollama API using the configured model and parameters. All LLM requests run asynchronously, ensuring no lag or blocking of the server.

5. **Response Routing**  
   Bot responses are routed back through the appropriate chat channel in game, whether it’s say, yell, party or general.

6. **Personality Management**  
   If RP personalities are enabled, each bot uses its assigned personality template. Personality definitions can be changed on the fly and reloaded live—no server restart required.

7. **Random & Event-Based Chatter**  
   In addition to responding to direct chat, bots will occasionally generate random environment-aware lines when real players are nearby, and will also react to key in-game events (e.g., PvP/PvE kills, loot, deaths, quests, duels, level-ups, achievements, using objects) using context-specific templates and personalities.
   Achievements get a gz only when they're worth one: a Feat of Strength, a realm first, one that gives a title, mount, pet or tabard, a meta-achievement like Glory of the Hero, or one worth 25 points or more. Nobody gz's Can I Keep Him? or Duel-icious, and a level achievement leaves it to the ding (`OllamaChat.Congrats.Achievement*`). The same goes in guild chat, which never hears of a ding at all, only of the Level 10, 20 ... 80 achievements: guild bots gz a ding only at 80 (`OllamaChat.Congrats.GuildLevels`). A random bot set up at its level is not a ding, so nobody congratulates that either.

8. **Live Reloading**  
   You can hot-reload the module config and personality packs in-game using the `.ollama reload` GM command or from the server console. All changes take effect immediately without requiring a restart.

9. **Fully Configurable**  
   All settings—reply logic, distances, frequencies, blacklist, prompt templates, chat history, personalities, random/event chatter, LLM params, and more—are controlled via `mod-ollama-chat.conf` and can be adjusted and reloaded live at any time.

## Personality Packs

`mod-ollama-chat` supports Personality Packs, which are collections of personality templates that define how bots roleplay and interact in-game.

- To use a Personality Pack, download or create a `.sql` file named in the format `YYYY_MM_DD_personality_pack_NAME.sql`.

- Place the `.sql` file in `modules/mod-ollama-chat/data/sql/characters/updates/`.

- The module will automatically detect and apply any new Personality Packs when the server starts or updates—no manual SQL import required.

Want to create your own pack or download packs made by the community?  

Visit the [Personality Packs Discussion Board](https://github.com/DustinHendrickson/mod-ollama-chat/discussions)

## Debugging

For detailed logs of bot responses, prompt generation, and LLM interactions, enable debug mode via your server logs or module-specific settings.



## Conversation Control

Bot chat can run away in several different ways, so there are four independent
brakes. All are configurable; see the `CONVERSATION GOVERNOR` section of the
config file.

| Brake | What it stops |
|---|---|
| **Chain depth + decay** | A bot replying to a bot replying to a bot. Each hop also multiplies the reply chance down, so chains lose energy before hitting the hard ceiling. |
| **The audience rule** | Bots holding conversations with nobody listening. Bots may only reply to *other bots* while a real player has spoken in that channel recently. This does most of the work. |
| **Cooldowns and rate limits** | One bot, or one crowd, dominating a channel. Per-bot, per-channel, and server-wide. The global limit also caps your LLM spend. |
| **Repetition scoring** | The same line twice, and the same *opening phrase* twice. Candidate replies are scored against the bot's own recent lines and the channel's recent traffic. |

If bots are looping, the setting to reach for first is
`OllamaChat.BotConversation.RequireRecentHuman`.

**Keeping the thread.** You rolled a 1, a bot said "1 out of 100 is a proper
joke", you typed "\*luck intensifies\*" -- and got "this place is a proper
maze". Two things went wrong, and both are fixed:

- A bot's unprompted line (ambient chatter, an ask) no longer lands in the
  middle of an exchange you have going. After a real player says something in
  a party, a channel, /say or guild chat, bots say nothing unprompted there
  for a minute, and a line that was written before you spoke is not sent at
  all (`OllamaChat.Context.AmbientHoldSeconds`).
- A short line means little on its own. The transcript had the roll and the
  joke, but further up, as background. Now a line of up to eight words is
  tied to what it came right after, next to the instruction to answer it:
  "Kami typed that right after this happened: loot roll for Gloves of the
  Fang ... Lanik won it, and then you said "1 out of 100 is a proper joke.",
  so it is most likely about that." (`OllamaChat.Context.LeadUpText`,
  `LeadUpMaxWords`). Game events in the transcript (a five-way roll runs
  long) are no longer cut off before who won.

## What Bots Talk About

Topics are chosen from weighted categories rather than uniformly, and each bot
suppresses whatever it used in its last few picks.

| Category | Default weight | Examples |
|---|---|---|
| **People** | 30 | Nearby players by name, class and what they are doing; group members; guildmates online; something the bot just watched happen |
| **World** | 30 | The nearest *interesting* creature (elite, rare, or a real level threat — never a critter, a guard or a townsperson); named NPCs by role; landmarks; corpses; time of day |
| **Activity** | 25 | Current quest objectives; danger assessment; group needs (someone low, someone out of mana) |
| **Self** | 15 | Spells, equipped items, bag space — the original topics, kept but demoted |

**Witnessed-event memory.** Bots keep a short memory of what they saw happen
near them — kills, deaths, level-ups, loot. This is what lets a bot comment on
the fight you were both just in rather than reciting a fact about itself.

**What a bot has been up to.** Each bot's last 45 minutes or so is kept: the
run of kills it was on (where, what and how many), the last quest it handed
in, the last level it reached. Ask a bot "what you up to?" and it answers with
what it actually did ("just been grinding murlocs in westfall") instead of
making something up (`OllamaChat.Lately.*`).

**Bots chatting among themselves.** In a city, now and then two or three bots
near you have a little chat in /say. There's one every 5 to 30 minutes at
random, at most one near you at a time, with an emote or none
(`OllamaChat.Mingle.*`). Each line follows on from the one before: a greeting
gets a greeting or a "whats up?" back, an answer is what that bot has really
been doing, and reactions and goodbyes are short and in each one's own style.
Questions get their question mark. Quests and items they ask about or show
off are linked, as players link them ("how long does [Are We There, Yeti?]
take?"), and they know what each one is: what they handed in or got lately is
described to them. Talk of someone's mount, pet or loot only comes up when
there is one: two bots on foot don't admire each other's mounts. Bots greeting each other without a word,
and bots waving at you as they pass, are rarer too: every 5 to 20 minutes and
4 to 12 minutes.

Note that `OllamaChat.Snapshot.IncludeSpells` now defaults to **0**. Listing
every off-cooldown spell a bot knew put dozens of lines of the most quotable
text in the prompt, which is why bots talked about their spellbook so much.

## Level, Class and What Bots Know

Every bot is told, in a sentence or two, what a player of its level does
(`OllamaChat.Outlook.LevelBands`): a level 6 is new and talks like it, a level
64 is levelling in Outland, and only a level 80 lives on heroics and raids. A
short list of tell-tale phrases (`Outlook.LevelPhrases`: "[40+] repair bill",
"[70+] heroic") is a backstop for when the model slips.
It is also told which roles its class can fill -- a mage never offers to heal
-- and that it knows other players only by what anyone could see of them:
their name, level, race, class, spec and guild.

Conversation premises can carry a level tag (`[80] getting into Ulduar runs`,
`[10-29] the elite around here that keeps killing people`), so a city
argument about Ulduar starts between level 80s.

**Which side.** A Horde bot in Orgrimmar asked General who was going AFK in
Stormwind, and bots there talked about the guards as if they were the
enemy's ("these grunts are too high level"). Every bot is now told which side
it plays and which cities are its own (`Outlook.SideText`,
`Outlook.Cities.Horde`/`.Alliance`), and while it stands in one of them --
the game's own say: the zone is a capital, and that side's -- that it is
safe there and the guards are on its side (`Outlook.HomeCityText`). Bots
chatting in /say are told the same (`Mingle.SideText`). Premises and pool
entries about one side's places and people carry that side's tag, `[horde]`
or `[alliance]`, and only that side's bots pick them: the `{city}`, `{spot}`,
`{zone}`, `{quest}`, `{npc}` and `{mount}` pools are tagged, so "the best
spot in {city} to stand around in" is never Stormwind for a Horde bot, and
nobody in Orgrimmar wonders what is down the Deeprun Tram. The guards
themselves are never "a creature close by". v25 left out creatures
friendly to the bot, but a city's people are friendly only to those in
good standing with it, and undead and blood elves start out only Neutral
with Orgrimmar: to them a grunt was still "around here, about level 75",
and they asked why it was. Now a place's guards and townsfolk, and a
training dummy, are scenery whoever is looking: never a topic, and never
listed in the reply snapshot. Bots still talk of the guards the way
anyone would ("they'll hold them off").

**Every level in a city.** Someone asking for a buff in Orgrimmar was told
they were "way too low level for this area". A bot standing in one of its own
side's cities is now told that people of every level come there
(`Outlook.HomeCityText`), and every bot that what someone is up to, and why
they are where they are, is their own business (`Outlook.KnowledgeText`).
Being told was not enough while the prompt itself said "a level 12 Gnome
Mage is near you": a bot in General asked why there was a level 12 there.
In a city (a capital, Dalaran or Shattrath) nobody's level is handed to a
bot as something it sees any more: a nearby-player line that names one is
not used there, and someone passing by or hanging out is described without
it.

**Levels in /say.** Standing near each other, anyone can see anyone's level
and class just by looking, so "i'm 54, just grinding" told to someone who can
see it is not what people say. A reply in /say is told so (`Say.SeenText`):
a bot gives its level only when asked, and hardly ever asks yours. Bots
chatting in /say among themselves are told the same (`Mingle.PromptTemplate`).

**What chat is like.** On a realm where nobody had typed in Trade all
evening, bots in Stormwind called it "basically a warzone", and one said
"brb gonna go check trade chat", as if it were somewhere to go. When the talk
is of chat (`Outlook.ChatWords`: "check trade", "general is dead", "lfg"), a
bot is now told that General and Trade are in everyone's chat window
wherever they stand, and how busy each has really been this past hour
(`Outlook.ChatText`, `Outlook.ChatStates`): the lines people typed there and
the lines bots said there, counted. Bots chatting in /say are told the same
every time (`Mingle.ChatText`), and "one of them is waiting for a group" is
gone from the /say premises: a bot in a queue never stands about chatting,
so it was never true.

**Nothing made up.** Asked about trinkets, a model will happily invent
"Trinket of the Hidden Sun", and "which race makes the best shaman" got
humans. Every bot is told (`Outlook.FactsText`) that anything from the game
it names -- item, quest, NPC, creature, boss, dungeon, zone, mount or pet --
must really be in Wrath of the Lich King, that it should say it can't remember
rather than make an answer up, and that made-up fun is fine when it is plainly
made up ("imagine if murlocs were ten feet tall"). Premises that need
something specific are handed the real thing from the game's own data
instead of a category to invent from: `{trinket}`, `{itemset}` and `{rare}`
near the bot's level, `{bosses}` after a dungeon or raid, and `{class_races}`
after a class (the races of the bot's own side that can be one). Races are
each side's own: a Horde bot talks about orcs and tauren, and mentions gnomes
or draenei only to run them down. Premises that could only be answered by inventing something
(the best prize from the Darkmoon Faire, a ninja-looting story with the item
in it) are gone.

**Quests, plainly.** "Kill 15 Blackrock Champions and Guard Howe in Lakeshire
will reward you" -- the quest log's own sentence -- had a bot tell someone to
kill the champions *and Guard Howe*. A quest is now described to the model by
its goals as the game lists them, each on its own, and who it is handed in
to: "kill 15 Blackrock Champions, then hand it in to Guard Howe". The quest
log's text is used only for a quest whose goals the game does not list (talk
to someone, go somewhere).

**"I need a wand."** Someone in Wailing Caverns said they still needed a
wand, and a bot sent them to kill murlocs past a big bridge that doesn't
exist. When a line says its speaker is after a kind of item -- a wand, a
shield, a cloak, boots, any of thirty kinds (`OllamaChat.Wants.*`) -- each
reply prompt is told what is really so for their level and class, from the
world database's loot tables: which boss in the dungeon they're in can drop
one (and whether it's down already this run), a dungeon or two around their
level where a boss can, what a profession makes, and whether the bot
answering could use one itself. Bots answer from that in their own style --
"i think mutanus drops one", "sfk has one off arugal", "hope one drops for
ya", "need one too lol" -- and a follow-up ("south of where?") gets the same
facts. The loot tables are read once, on the first such line.

**Passers-by.** A bot remarking on someone it walks past is told the one piece
of what they wear that catches the eye, so a compliment has something real to
go on, and a remark about a mount to someone on foot (or a pet to someone with
none out) is never said: one told to pay a compliment with nothing to go on
admired a mount nobody was riding.

## Battle Cries, Race Talk and Banter

**Battle cries.** Shout "FOR THE HORDE!" in General and the Horde shouts it
back; shout "for the alliance" there and you get booed, called a traitor and
asked whose side you are on, all in fun. A real player's short line that is
a side's battle cry (`OllamaChat.Sides.Cries.*`: "for the horde", "lok'tar
ogar", "for the alliance") is said to everyone, and each bot answering is
told whether it is its own side's (`Sides.OwnCryText`) or the other side's
(`Sides.EnemyCryText`).

**What each race says.** Bots knew their race only as a word. Every race now
has its own phrases from the game itself -- its NPCs' greetings and
farewells and its language's best-known words (`OllamaChat.Race.Phrases.<Race>`):
"zug zug" and "lok'tar" for orcs, "hello mon" and "stay away from the voodoo"
for trolls, "ishnu-alah" for night elves, "bal'a dash, malanore" for blood
elves, "walk with the Earth Mother" for tauren, and so on. Say one and the
bots know what it is and whose it is, an orc answers your "zug zug" first if
one is about, and saying it back is part of the joke (`Race.HeardText`). Now
and then -- 3% of a bot's lines, a quarter of the roleplayer's
(`Race.UseChance`, `Race.StyleChance`) -- a bot is reminded of its own and may
slip one in, and two orcs meeting in /say might zug zug each other
(`Mingle.RaceText`).

**Banter that bites back.** A bot called a noob answered "Fair point, I'll
keep farming". A real player's dig -- "noob", "ur mum", "l2p", or any of
`Response.InsultWords` (`OllamaChat.Banter.Words`) -- now tells each bot
answering that it is banter, not a real complaint, and to give as good as it
gets in its own style, with a different kind of comeback each time
(`Banter.Text`). Someone who has said they feel picked on gets none of it for
a while (`Gear.BackOffWords`).

## Item and Quest Links

Bots are asked to put exact item and quest names in square brackets, and each
`[Name]` becomes a real chat link when the line is said: the link a player
posted lately if there is one, else a quest in the bot's log or an item it
carries, else the item and quest tables (nearest the bot's level, and its
faction for quests). Unknown names just lose their brackets. See
`OllamaChat.Links.*`.

## Stock Phrases

A phrase on `OllamaChat.Response.OverusedPhrases` ("taking forever", "such a
grind") may be said once per `OverusedCooldownSeconds` across the whole realm;
a line that would repeat it sooner is dropped. Things that never existed in
Wrath (`OffEraPhrases`: keystones, transmog, world quests) are never said. A
chat style's hobby horse (`RarePhrases`: "40 man", "back in vanilla") gets
through once an hour, even in a reply owed to someone.

**The fourth wall.** As far as any bot knows, everyone on the realm is a player
like itself, so no bot talks about bots, AI or "real players"
(`FourthWallPhrases`). It's a rule that can be broken, but only when a perfect
opportunity appears: a real player bringing it up ("are you a bot?"), never
another bot, and then only the first answer gets to wink at it; nobody else
does for `FourthWallMinutes`. Names that only look like it, such as Field
Repair Bot, Clockwork Rocket Bot and Mimiron's bomb bots, are fine anywhere
(`FourthWallAllow`).

`DropVocatives` takes "kid" out wherever it addresses someone ("just cave mobs
kid" goes out as "just cave mobs"), and a thank-you on its own ("ty", "cheers
mate") gets no reply unless it asks something too (`Crowd.ThanksReplyChance`).
`Swaps` says a phrase people have stopped saying the way they say it now:
"Fair point, but..." goes out as "Fair, but...", while "that's a fair
point" stays as it is. Add your own as `old=new`, or `old=new,other` for a
pick.
A dead bot is told it is dead, so what it says fits.

## Food and Water from Mages

Ask a mage bot in your group for water the way you would ask a person --
"water pls", "mage can i get food", "anyone got water?", "Frostiq please can
you give me mana pots" -- and it conjures the best food or water you can use
at your level and opens a trade with it already in, its side accepted. Your
accept finishes it, and it never asks for gold. It answers in its own chat
style.

When it can't, it says why (not a mage, not learned it yet, bags full); when
it only can't yet (a fight on, out of mana, too far apart to trade) it says it
will, and does as soon as it can. A trade window you already opened with it
gets filled instead. Someone outside its group can ask too (see Trading
with Strangers below). "need mana", "oom" and "mb" stay mana breaks, a bare
"pots" needs a real ask ("pls send me pots", since "pot now" is a raid call),
and playerbots' own commands ("t water", "use water") are left to playerbots.
See `OllamaChat.Conjure.*`.

## Buffs from Strangers

Ask for a buff in /say the way you would ask anyone -- "can i get a buff",
"buff pls", "anyone got fort?", "kings please", "Verona int pls" -- or
whisper a bot for one, and a bot standing near that can put one on anybody
might: a priest Power Word: Fortitude, a mage Arcane Intellect (to anyone
with mana, or anyone who asks for it by name), a druid Mark of the Wild, a
paladin Blessing of Kings, or Might or Wisdom before it has Kings. Divine
Spirit, Shadow Protection, Thorns and Sanctuary come when you name them. It
really casts it, a moment later -- off its mount first, with its own mana --
and the core gives a low-level player the rank they can have.

Whether it does is up to its chat style (`OllamaChat.Buff.Style.<KEY>`): it
might buff you and say so, buff you without a word, say no, or let it go by.
A friendly one nearly always helps; a rude one often won't. Asked of
everyone, at most two bots buff you, each with something different. Asked of
one bot -- by name, by whisper, the one you have targeted, or the one you were
just talking to -- it is that bot's to answer, and one that can't may say
why: it's a warrior and its shouts only reach its own group, it's out of
mana, you have it already. Asked for another class's buff, it puts its own
on you instead. Say "ty" after and the one who buffed you answers ("np"), or
doesn't, and nobody else chimes in: that's the end of it.

Bots in your group are left to playerbots, which keeps its own group buffed
("buff us" in party chat is an order), and nobody flagged for PvP is buffed
by a bot that is not. `.ollama status bots` counts asks, casts, refusals and
thank-yous. See `OllamaChat.Buff.*`.

## Trading with Strangers

Playerbots turns down any trade with someone outside a bot's group ("I'm
kind of busy now"), so these used to need an invite first. Now someone
outside the group can ask too -- by name, by whisper, or, for water, of
everyone near in /say ("anyone got water?") -- and the bot takes it as a
favour, its own way, by the same chat style as a buff
(`OllamaChat.Buff.Style.<KEY>`): it does it and says so, does it without a
word, says no, or lets it go by.

- **Water and food from a mage** outside your group: it conjures it and opens
  the trade, as for its own group.
- **Something you sell or give it** ("wts [Pagan Vest] 5g", whispered): it
  says what it would pay, or that it would like it, and when you open a trade
  and put the thing in, it puts in the money and accepts its side.
- **Something spare from its bags**, with `OllamaChat.Bags.GiveAway = 1`
  (what it offered you always goes over).

While such a trade is on, or about to be (you're close, with water waiting or
a price agreed), the bot's playerbots AI is held from the world tick, so it
never sees the trade to turn it down, and its side is accepted for it; a
fight calls it off. `OllamaChat.Favor.Strangers = 0` puts things back: an
invite first. Bots only answer ordinary whispers with
`OllamaChat.EnableWhisperReplies = 1`; asks for buffs, water and spare things
are answered either way.

## Orders in Plain Words

Tell the bots what to do the way you would tell anyone, in party, raid or a
whisper, and the one it was for does it and says so in its own words:

| Say | They |
|---|---|
| "stay back", "wait here", "hold position" | stay put ("ok, shout when you want us following") |
| "follow me", "come with me", "on me", "get back here" | follow |
| "attack my target", "kill that", "focus it" | attack what you have targeted |
| "run!", "fall back", "get out of there" | break off and come back to you |
| "release", "res", "respawn", "run back" (to a dead bot) | release and run back |
| "Joanni come trade me", "open a trade" | walk over and offer a trade |
| "hearth out", "use your hearthstone" | hearth home |
| "you're stuck", "use unstuck" | the stuck tool: dungeon entrance, or the nearest graveyard |
| "buffs pls", "rebuff" | buff the group |
| "pull", "pull when ready" | (the tank) pull your target, or the skull |
| "Joanni leave the group" | leave |

Name a bot ("Zonin stay", "Frostiq and Zonin wait here") to order only that
one; otherwise every bot that takes orders from you does it. Hearth needs a
name, a whisper or "everyone". Questions, negations and conditions are not
orders ("should we wait here?", "dont stay here", "if it runs, kill it"), and
neither is talk that only mentions one ("we should stay here and fight") or
is about you or someone else ("ill wait here", "gonna hearth"). Nothing is
ever summoned. See
`OllamaChat.Orders.*`. When a dungeon-clear run starts or carries on, any bot
still told to stay comes along.

Playerbots also reads every chat line as one of its own commands unless
`Playerbots.CommandPrefix` is set: then "leave it" makes a bot leave and "go
ahead" sends every bot off. Setting the prefix (`"!"`) leaves plain chat to
chat and orders, with playerbots' commands still there as `!summon`.

## Greetings, Mana Breaks and the Finish

A bot tank leading a dungeon-clear group says
to let it know when everyone is ready as the group comes in, and says so when
every boss is down (`OllamaChat.Party.Greet*`, `Party.Done*`). "im ready now"
starts a run, and so does "lets go" however it's typed: "lego", "lesgo",
"letsgoooo", "leeggoooo". A word typed long for effect counts as itself, so
"readyyy" is "ready".

If nobody answers for a minute and the whole group is in, alive and topped
up, the tank takes that as a yes: it says so in its own way ("ok, taking
that as a yes, pulling") and starts. Anything said in party after it
asked, "brb" or "one sec" or just chat, leaves the start to you
(`Party.ReadyTimeout*`).

Call a mana break ("mb", "need mana", "drink", "lets eat") and the tank stops
with a quick "np" or "drink up", then keeps an eye on the bars, like a real
tank. When everyone has their health and mana back, it says it's carrying on
and does, in its own style: "right lets go guys" from one, "too slow, lets go"
or "KEEP UP" from another. Your mana and the healers' have to be all but full,
the other bots' most of the way, and nobody can still be sitting down
drinking. "ok im done", "full" or "ready" still carries on at once. A "brb",
"wait" or "stop", during a break or on its own, waits until someone says
they're ready (`OllamaChat.Party.BreakEnd*`). A hunter or shaman bot only needs
a quarter of its mana: both do their damage without much of it.

## Skipping a Boss

"skip the next boss", "can we skip oggleflint", "skipp him" or "skip jergosh
and bazzalan" in party skips them: the run leaves them out, and the tank says
so in its own style, and where it's heading now. Names are read the way
people type them ("oglefint", "taraga", "van cleef", "cobrahn" for Lord
Cobrahn). "skip to taragaman" or "straight to the last boss" skips every boss
before it. "dont skip jergosh" or "actually lets do jergosh" takes a skip back:
a boss the run has already passed is gone back for now ("...later" leaves it
to the end), one ahead comes in its turn. Asked before the run starts ("ready,
skip the first boss"), the skip waits for it. A boss already dead or being
fought gets a no and why. Questions ("why did we skip him?") stay chat. While
bosses are skipped, the bots know it, and the end of the run isn't called a
full clear. Skipping a boss another one needs (one of Wailing Caverns' four
Fanglords, which Mutanus needs) leaves the run stuck later: take the skip back.
Needs mod-dungeon-clear with named skips (`dc skip <boss>`); see
`OllamaChat.PartyCommands.Skip*`.

## New Gear

A bot in your group that puts on a rare or epic it just got (loot, a roll it
won, a quest reward) may say so in party in its own words: "aww yes, [Gloves
of the Fang] going straight on". Whether it does depends on its chat style:
an excitable one usually will, a quiet one or a loner never does. Gear
playerbots hands it doesn't count. See `OllamaChat.Equip.*`.

## A Potion in a Pinch

A bot in a dungeon that uses a healing potion with its health nearly gone (35%
or less) -- the tank on its last legs -- or a mana potion with its mana nearly
gone (20% or less) -- the healer running dry -- and comes through the fight
may say so in party once it is over: "that pot saved my arse", "mana pot
saved us there", "too close lads". It is told who it is in the group (tank,
healer or damage), where, what it used and how low it was, and says it its
own way, with a different angle each time. How likely depends on its chat
style -- the hype merchant nearly always, the quiet one hardly ever -- and
not when it died anyway. Healthstones and mana gems count: what matters is
what the game says the item does. See `OllamaChat.Potions.*`.

## Giving Things Away

Players don't hand their things to strangers for nothing, so bots don't
either. Asked for something they have no use for, they say no, and they never
offer things free in chat. Their own supplies (ammo, food and drink, potions,
reagents, sharpening stones, oils and poisons, which playerbots gives them)
are never handed over. `OllamaChat.Bags.GiveAway = 1` brings free gifts back.

## With mod-playerbots-plus

[mod-playerbots-plus](https://github.com/kamakazedan/mod-playerbots-plus) has
random bots do what players do that needs no language model: trade on the
auction house, hang out where players do in the cities, stop getting on and
off their mount at a herb they can't pick, get back up after dying, and use
raid marks. It also mutes playerbots' stock whispers, like "Using
[Hearthstone]". City hangouts, stuck mounts, getting back up, raid marks and
the muted lines used to be part of this module. Their settings are in its
conf now, renamed from `OllamaChat.*` to `PlayerbotsPlus.*`.

With both modules in the same build, the bots talk about what it has them do:

- **The auction house:** a bot that just sold something, bought something,
  put something up or got something back unsold may bring it up in chat once
  within the next hour and a half: "finally sold those [Linen Cloth]", "got
  [Shadowfang] off the AH for 30g, bargain". See
  `OllamaChat.Topic.WeightMarket`, `Market.TalkMinutes` and
  `EnvCommentMarket*`.
- **City hangouts:** a bot hanging out knows where it is and what it's doing
  when someone asks. An AFK one stays quiet until someone talks to it, then
  it's back (`OllamaChat.Hangout.StateText`, `AwayText`).
- **Stuck on a mount:** a bot that was stuck knows it if asked, and may laugh
  it off (`OllamaChat.Stuck.*`).
- **Pets, companions and mounts:** random bots' hunters name their pets the
  way players do (Aslan, Mr Bigglesworth), and bots own and walk companion
  pets that suit their side and level. Ask a bot about its pet ("cute cat,
  whats it called?") and it knows what the pet is and what it's called, and
  what is trotting along behind it; say "nice mount" and it knows whether
  it's riding one (`OllamaChat.Pets.*`). Bots chatting among themselves see
  each other's mounts, pets and companions too, each named as what it is: a
  hunter's wolf is a pet, a warlock's imp a demon, and neither is a mount.
- **Duels:** bots gather outside the gates of Orgrimmar and Stormwind and
  duel each other there, as on any realm. The chat side is the usual duel
  banter, and a bot at the gates knows it is there watching the duels. Two
  bots chatting in the city may talk themselves into a duel -- only two of a
  side within a few levels of each other, near a city whose gates have a
  duel spot (`[duel]` in `OllamaChat.Mingle.Premises`,
  `Mingle.DuelChance`) -- and when they're done talking they walk out to the
  gates and fight it out there. If the crowd at the gates is full, two of it
  head off to make room. Anywhere else, nobody talks of a duel.
- **Raid marks:** a rogue that can't sap the star says why in party
  (`OllamaChat.Marks.Say*` and the `Sap*` situations).
- **Group quests:** bots getting a party together for a group quest say so
  in General, each in its own style: the ask for help ("LF2M [The Ogre
  Threat]", the quest linked), the answers of those who have it too, the one
  getting the party together telling someone who asks to come along, and
  going it alone when nobody came. Only where a real player is in General to
  read it, and an ask never talks over one (`OllamaChat.GroupQuests.*`).
- **City life:** the crowd hanging out in a city shifts about, hops, dances,
  runs errands, fishes and flies in and out (mod-playerbots-plus's
  `PlayerbotsPlus.CityLife.*`). When someone gets on a mount few people
  online have, a few wander over for a look and one of them says something
  about it, the real mount by name. A bot that starts dancing, or joins
  someone dancing, does it with the emote. Bots talking in /say stay put
  while they do, look round at whoever speaks near them, and walk over to
  talk face to face (`OllamaChat.CityLife.*`).
- **Muted lines:** this module has the bots say "Staying", "Following", hello
  on joining a group, and a quest objective done in their own words. Give
  mod-playerbots-plus the longer mute list from its conf, so playerbots' own
  versions of those lines are muted too. Anything this module sends for a
  bot is never muted.

Each module builds and runs without the other. Without mod-playerbots-plus,
none of the above happens and the rest of this module works as before.

## Per-Style Nudges and Longer Replies

Any chat style can have `OllamaChat.Personality.Nudge.<KEY>`
(`"90|instruction|otherwise"`: an extra instruction on that share of its
replies) and `OllamaChat.Personality.MaxReplyLength.<KEY>` (a longer reply
limit, for a style that explains things). A personality prompt alone tends to
get all or nothing from a small model; a nudge rolls for each reply.

Typed lines -- a "gz", a "ty all", a wipe's "my bad", the tank's "ok lets go"
-- are written once for everyone, so each style types them its own way
(`OllamaChat.Personality.CannedStyles`): the proper speaker says
"Congratulations.", the abbreviator "gz", the lowercase typer drops the
capitals, the excitable one adds a "!". Everything the model writes already
goes through the bot's own personality prompt.

## Memory and Relationships

Conversation history is a sliding window. Once a line falls out of it the bot
has no idea it ever happened, which is why bots otherwise feel like they meet
you fresh every session. Two mechanisms give them continuity, both bounded so
the prompt never grows without limit however long a character has been alive.

**Memory.** History accumulates until it crosses a token budget. At that point
the model condenses it into a handful of short narrator-style notes, each
scored 1-10 for importance, and the raw history is cleared. At prompt-build
time the most important notes are selected within a separate, smaller budget.
A character gradually accumulates what mattered and forgets the small talk.

**Relationships.** When a name comes up often enough in a bot's history, the
model is asked to write -- or revise -- a sentence on how that bot feels about
that person. That sentence goes into future prompts, so a character's attitude
toward you persists and evolves rather than resetting. Whoever the bot is
currently talking to is always listed first, so their own relationship never
gets squeezed out by the budget.

Both work in normal mode and in roleplay mode. They complement the numeric
sentiment score rather than replacing it: sentiment is how warm the bot feels,
these are *why*.

Condensation and relationship writing are themselves LLM calls, so they run on
the dispatcher's workers with a stricter queue allowance than replies get --
background upkeep can never crowd out live conversation.

Requires `data/sql/characters/base/2026_08_29_memory_relationships.sql`.
Tune it under the `LONG-TERM MEMORY AND RELATIONSHIPS` section of the config.

| Setting | Default | Effect |
|---|---|---|
| `Memory.HistoryTokenLimit` | 1500 | How much history accumulates before it is distilled |
| `Memory.PromptTokenBudget` | 400 | Hard stop on how much of the prompt memories may use |
| `Memory.MaxPerBot` | 40 | Notes retained per character; least important dropped first |
| `Relationship.MentionThreshold` | 8 | How often a name must come up before an opinion is written |
| `Relationship.MaxPerPrompt` | 3 | Relationships included per prompt |

## Roleplay Mode

Off by default. Turn on with `OllamaChat.Roleplay.Enable`.

Gives each race a speech register and cultural touchstones, and each class a
worldview — what that character *notices*. A Tauren speaks slowly of the
Earthmother and the balance; a Forsaken is dry and calls the living "breathers";
a priest sees wounds, a hunter reads tracks, a rogue counts exits.

`OllamaChat.Roleplay.Strictness` controls how far it goes:

- **0** — Flavour only. Voices colour the prompt, nothing else changes.
- **1** — In character. Out-of-world vocabulary (`dps`, `nerf`, `patch`, …) is
  rejected rather than spoken, and faction attitude is applied.
- **2** — Hard in character. In-world chatter lists replace the shipped
  out-of-character ones, injuries and distances are described rather than
  quoted as figures, and think mode is used where the model supports it.

`OllamaChat.Roleplay.CrossFactionGibberish` (on by default in roleplay mode)
stops bots answering across factions in say/yell — the client renders those as
gibberish anyway, so a fluent reply is the most immersion-breaking thing the
module can do.

Race and class voices can be overridden per server in the
`mod_ollama_chat_voice` table without a rebuild.

## Body Language

Replies are accompanied by movement so they read as conversation rather than as
a log line.

- **Facing** — the bot turns toward whoever it is answering, just before the
  line lands. Skipped while moving, casting, in combat, in flight, on a
  transport, or teleporting.
- **Gestures** — the model may end a reply with `[emote:wave]`, `[emote:nod]`
  and similar. The tag is parsed out and stripped before the line is spoken.
  `*waves*` and a bare `/wave` are also recognised, because models emit those
  unprompted. Emotes are played through the same code path the client uses, so
  nearby players see both the animation and the "Bot waves at You." social text,
  with the correct per-race and per-gender variant.
- **Emote reactions** — bots react when a player emotes at them, either by
  mirroring (wave gets a wave), countering (flex gets a laugh), or answering in
  words. The first two cost no LLM call.
- **Emotes are remembered** — a player's emote at a bot, the bot's emote back,
  and a bot's emote at someone walking past all go into the chat history. Ask
  "why you sighing at me?" and the bot that sighed answers, knowing what it did.
  Emote at a bot and then say something, and that bot takes it as said to it
  (`OllamaChat.Emote.EventText`).

## Think Mode

`OllamaChat.ThinkMode` takes `auto` (default), `on`, or `off`.

Under `auto` the module asks Ollama what the configured model can actually do
and only ever sends `think` to a model that reports the capability — so a model
that cannot think is never asked to. It then spends reasoning only where it
changes the answer: off for short chat lines, on for sentiment analysis and
strict roleplay replies.

If a live request is ever rejected for asking to think, the module remembers
that, logs it once, and retries without it. A latency guard
(`OllamaChat.ThinkMaxLatencyMs`) backs think mode off for the session if it
proves too slow for chat. You do not need to restart after swapping models —
`.ollama reload` re-probes.

With a non-Ollama provider (`OllamaChat.Provider = openai` or `anthropic`)
nothing is probed, because the only way to ask would be a billed request.
`auto` then never requests reasoning; `on` requests it on every call and backs
off for the session if the provider rejects it.

## Threading Model

Worker threads do HTTP and string work only. Every read or write of a `Player`,
`Channel`, `Guild`, `Group` or `Map` happens on the world thread.

Requests are built on the world thread, handed to a bounded worker pool
(`OllamaChat.WorkerThreads`), and delivered back on the world thread by a
completion queue drained each tick. Queue depth is capped
(`OllamaChat.MaxQueueDepth`) so a slow Ollama sheds load instead of building a
backlog of stale replies.

## License

This module is released under the GNU GPL v3 license, consistent with AzerothCore's licensing.

## Contribution

Developed by Dustin Hendrickson

Pull requests, bug reports, and feature suggestions are welcome. Please adhere to AzerothCore's coding standards and guidelines when submitting contributions.
