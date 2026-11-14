# CLAUDE.md — mod-ollama-chat

Guidance for working in this module. The repo-level `CLAUDE.md` at the
AzerothCore root still applies; this adds module-specific rules.

## The one rule that matters most

**Worker threads do HTTP and string work only. Every read or write of a
`Player`, `Channel`, `Guild`, `Group` or `Map` happens on the world thread.**

This module talks to a network service, so it is permanently tempting to "just
do it on a background thread". Don't. AzerothCore's world state is not
thread-safe, and violating this produces intermittent crashes that are extremely
hard to attribute back here.

The module previously spawned one detached `std::thread` per bot per message and
called `ObjectAccessor`, `Channel::Say`, `botAI->Say` and the whole eligibility
scan from it. That is what `mod-ollama-chat_dispatch.{h,cpp}` exists to prevent.

How to add a new kind of bot utterance:

1. Build the prompt **on the world thread**, where the `Player*` is live.
2. Fill an `OllamaChatRequest` — resolve every value you need (names, guids,
   scope key) into it now, so the worker never has to touch a `Player`.
3. `OllamaDispatch_Submit(std::move(request))`.
4. Delivery happens for you in `OllamaDispatch_Update()`, on the world thread.

Do **not** call `QueryOllama()` directly from anywhere that could be the world
thread — it blocks for a full LLM round trip.

A line nobody asked for (ambient chatter, an ask) sets
`OllamaChatRequest::writtenAtSeq = Context_NextSeq()` and checks
`Context_HumanSpokeSince` before it picks a place to go: one written before a
real player spoke there is dropped at delivery, and none starts within
`Context.AmbientHoldSeconds` of one. A new kind of unprompted line does both,
or it talks over people.

A hook that fires on a map thread (a kill, loot, a death, a ding, an
achievement, gear put on, a potion) notes what happened under a mutex and
nothing more; the world tick acts on it (`Events_Update`, `Equip_Update`,
`Potions_Update`, ...). Event chatter used to build its prompts in the hook
itself, on whichever map thread raised it -- with five map threads running at
once, a guild event read bots anywhere in the world from there. The one hook
that answers where it is, `ChatOnEmote` (a text emote is handled on the map
thread), reads only the two players on that map and state behind a lock --
the personalities included (`g_BotPersonalityMutex`).

`EventProcessor::AddEvent` (`bot->m_Events`) is **not** a way around this. It
mutates a container that `Player::Update` walks, so calling it off-thread is the
same race it looks like it avoids.

A dungeon run by bots with nobody playing among them (mod-playerbots-plus's
bot dungeons, or any group of bots the dungeon finder made) is silent:
`OllamaInBotOnlyDungeon` drops every line from it that is not an answer owed to
someone who spoke to the bot, at submit (no model call) and again at delivery.
A new kind of line needs nothing for this; it goes through both.

## The second rule: workers never read config strings directly

A worker thread must not touch `g_OllamaUrl`, `g_OllamaModel`,
`g_OllamaSystemPrompt`, `g_OllamaStop`, `g_OllamaSeed`, `g_OllamaApiKey`,
`g_SentimentAnalysisPrompt` or any other `std::string` global.

`.ollama reload` reassigns those on the world thread. Reassigning a
`std::string` frees the old buffer, and a worker copying it at that moment
dereferences freed memory. This produced a real crash in the wild: an
ACCESS_VIOLATION deep inside cpp-httplib's header handling, with a stack that
pointed at the HTTP client rather than at the actual cause.

The pattern to follow:

- endpoint settings go through `OllamaConfig_Snapshot()`, published under a
  mutex by `OllamaConfig_Publish()` at the end of `LoadOllamaChatConfig()`
- anything else a worker needs is formatted on the world thread at submit time
  and carried in the task (see `BuildSentimentPrompt`,
  `Memory_BuildCondensationPrompt`)

POD globals (`bool`, `uint32_t`, `float`) are a benign racy read and are fine
to touch directly.

## Provider support lives in one file

`OllamaChat.Provider` (`ollama` / `openai` / `anthropic`) only changes how a
request is encoded and decoded inside `mod-ollama-chat_api.cpp`: one builder,
one parser and one header function per provider, selected on
`OllamaEndpointSettings::provider`. Nothing outside that file should branch on
the provider except the capability probe, which is skipped for non-Ollama
providers because a live probe there is a billed request. Add a provider by
adding a builder/parser pair and an enum value; do not teach callers about it.

## Channels are resolved by id, never by name

Zone channels are named `"General - Elwynn Forest"`, so
`ChannelMgr::GetChannel("General")` always returns nullptr. City channels
(Trade, GuildRecruitment) only exist in cities. Names are localized, so any
substring test on them breaks on a non-English realm.

Use `OllamaResolveZoneChannel(bot, ChatChannelId::X)`, which matches on channel
id plus zone-name containment the way `PlayerbotAI::SayToChannel` does, and
carry the channel's *actual* name and id into the request. Getting this wrong
silently loses ambient chatter, and also splits the governor's scope key so
ambient lines and replies in the same channel stop sharing a cooldown and a
repetition history.

## Script registration facts (verified against this fork)

- `PlayerScript(name)` with **no hook list enables every hook**. See
  `PlayerScript::PlayerScript` in the core: *"If empty - enable all available
  hooks."* Passing an explicit list is a performance nicety, not a requirement.
  Do not "fix" a bare `PlayerScript("Name")` thinking it registers nothing.
- Always write `override` on hook overrides. Two hooks in this module silently
  overrode nothing for a long time because they lacked it:
  - `OnPlayerCompleteAchievement` — the core's name is
    `OnPlayerAchievementComplete` (word order).
  - `OnGameObjectUse` — not a `PlayerScript` hook at all.
- There is no generic "player used a gameobject" hook. `ChatOnGameObjectUse`
  uses `AllGameObjectScript::CanGameObjectGossipHello` and returns `false` so
  normal handling continues.
- There is no `OnGuildMemberLogin`. Guild login uses `PLAYERHOOK_ON_LOGIN` plus
  a guild check; promote/demote come through `GuildScript::OnEvent`.
- Read a looted item in `OnPlayerStoreNewItem` (as `ChatOnLoot` does), never
  in `OnPlayerLootItem`. Hooks run in module order, and mod-junk-to-gold sells
  and destroys a bot's grey loot inside its own `OnPlayerLootItem`, before
  this module's: a new item destroyed is deleted there and then, so the
  `Item*` a later hook gets is freed memory. Following it in
  mod-playerbots-plus's ledger brought the server down within minutes of
  the bots coming online.
- `urand(min, max)` is **inclusive on both ends**. `urand(0, 100) > 0` fires
  about 1 in 101 times, so a configured chance of 0 is not "never". Use
  `urand(1, 100) > chance` and check `chance <= 0` first.

## Driving playerbots from this module

- Chat hooks (`CMSG_MESSAGECHAT` is world-thread) and `OnWorldUpdate` run while
  the map threads are idle, so touching a bot's AI there is safe; nowhere else.
- Send a bot a command with `PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER,
  sPlayerbotAIConfig.CommandPrefix + cmd, player)`: playerbots' own security
  checks apply and it runs in the bot's next AI tick. With the prefix empty,
  playerbots has already read the player's line as a command of its own
  (`ExternalEventHelper::ParseChatCommand`: whole line, then shorter word
  prefixes), so a command meant to override it is queued from the world tick,
  after it (`mod-ollama-chat_orders.cpp`).
- `"buff"` is playerbots' list of buff potions; class buffs are `"rebuff"`.
  `"release"` on a living bot makes it stay. `"revive"` is the spirit healer.
- A buff asked of a stranger (`mod-ollama-chat_buffs.cpp`) is never a
  playerbots command -- a random bot takes none from someone it does not
  serve -- but the bot's own cast, from the world tick: `Player::CastSpell`
  with the best rank it knows (the core scales the aura down for a
  low-level target), off its mount and out of a form that blocks it first.
  Whether a buff would stack is the core's own spell groups
  (`SpellMgr::CheckSpellGroupStackRules`), not a list here. An ask it
  answers is claimed, whoever acts on it, so no crowd chats over it.
- Playerbots turns down every trade with a person outside the bot's group
  (`TradeStatusAction`, "I'm kind of busy now"), from the trade packets it
  queues for the bot's next AI tick. A trade made as a favour to someone
  outside the group (`mod-ollama-chat_favor.h`: conjure, bags, gifts) holds
  the bot's AI with `Favor_HoldAi` every world tick while the trade is open
  or about to be, so that tick never comes; the module accepts the bot's side
  itself, and the queued packets find no trade once the AI is back. Hold it
  with `YieldThread` (never shorter than a hold someone else set, such as a
  city hangout's), call it off in a fight, and never hold a bot for a trade
  that is not one of this module's. How the bot takes the ask is its chat
  style's (`Favor_Roll`), the same weights as a buff.
- Packet hooks (`ServerScript::CanPacketSend`) run on any thread: read only a
  snapshot published from the world thread (mod-playerbots-plus's
  `PlayerbotsPlusMute.cpp` does).

## What bots do without a model lives in mod-playerbots-plus

City hangouts, stuck mounts, getting back up, raid marks, muting playerbots'
stock lines and the auction house market are mod-playerbots-plus's. This
module only voices them. Do not add bot behaviour that needs no language model
here: put it there and raise an event.

The one exception is steering a mod-dungeon-clear run from what a player types
(`mod-ollama-chat_partycmd.cpp`): reading "mb" out of a party line is this
module's, so the pause, the resume, and the end of a rest that ends by itself
(the tank's "lets go" goes out first, and the run moves when it lands) stay
together here, and so does skipping a boss someone asks to skip. It is only
ever the tank's own `dc` actions, sent as the player (`dc skip <boss>`,
`dc unskip <boss>`, `dc go <boss>`: what they take is mod-dungeon-clear's
`DcCommandApi.h`), never its run state written from here; the skip list is
read back to see it took. mod-playerbots-plus knows nothing of
mod-dungeon-clear.

- Include `mod-ollama-chat_plus.h`, never `PlayerbotsPlus.h` directly.
  `OLLAMA_CHAT_PLAYERBOTS_PLUS` is its API version, 0 when it is not in the
  build, and this module must compile and run either way.
- Its events arrive on the world thread. A `PlayerbotsPlus::BotEvent` comes
  while the bot is still where it happened: look it up by guid, then build the
  prompt as anywhere else (`mod-ollama-chat_stuck.cpp`, `_marks.cpp`). A
  `MarketEvent` may be about a bot that is offline (a sale), so
  `_market.cpp` only notes it. What the bot says, and its prompt settings,
  stay here.
- `mod-ollama-chat_hangout.cpp` and `MuteOwnLine` are thin bridges with stubs
  for a build without it, so their callers need not care.
- `Revive_RunDecides` and `Revive_KnowsResurrect` are this module's own copies:
  orders and the wipe lines need them with or without mod-playerbots-plus.
- A duel two bots talk themselves into (mingle's `[duel]` premise) is only
  ever talked of when `Hangout_CanGoDuel` says they could go (API 3), and
  `Hangout_GoDuel` at the end of the exchange hands them over: the walk out
  to the gates and the duel are mod-playerbots-plus's.

## Performance traps specific to this module

- Never walk `Map::GetCreatureBySpawnIdStore()` — it is every spawn on the map.
  Use `Cell::VisitObjects` with a searcher, as `topics.cpp` and `handler.cpp`
  now do.
- `Acore::AnyUnitInObjectRangeCheck` filters out anything not alive. Where dead
  creatures matter (corpse topics), use the local `NearbyCreatureCheck` structs.
- Watch for nested `ObjectAccessor::GetPlayers()` loops; that is quadratic in
  online characters, per message.

City life (`mod-ollama-chat_citylife.cpp`) is mod-playerbots-plus's too:
the crowd hanging out shifting about, hopping, dancing, errands, fishing,
flights, a few wandering over to look at a rare mount. Here only what is
said of it (a remark on the mount, the real one by name, from its
`BotEventType::CityMountShown`; the dance emote, from `CityDance`, played
with `stateKept`, since the dance and its end are plus's), and what plus is
told so nobody walks off mid-sentence: `Hangout_Busy` (a bot talking or
emoting), `Hangout_Spoke` (someone said something in /say: those about look
round), `Hangout_Gather` (a /say exchange between bots: they walk over to
talk face to face). A /say line delivered by the dispatcher and an emote
played with `BotPlayTextEmote` already do it; a new way of saying or emoting
that goes round them must too, or the bot talks while it walks off.

Group quests (`mod-ollama-chat_groupquests.cpp`) are mod-playerbots-plus's:
who asks for help, who comes along and who leads. Here only their lines, from
its `BotEventType::GroupQuest*` events, in the zone's General channel. They
go out with `triggerBotReplies` off: an answer another bot made up ("I have
it too!") would not be one of the party plus is getting together. An answer
or invite is said only after the ask it answers went out to be written, and
carries `writtenAtSeq` too, so a player speaking drops both: otherwise a held
ask left its answers replying to nothing.

## Prompt content

Whatever is most concrete and quotable in the prompt is what the model will
talk about. Historically the module pasted every off-cooldown spell a bot knew
into the prompt, which is exactly why bots kept talking about their spellbook.
`OllamaChat.Snapshot.IncludeSpells` defaults to `0` for this reason.

When adding prompt material, prefer things outside the bot — people nearby,
what just happened, where they are — over facts about the bot itself. The
weights in `OllamaChat.Topic.*` encode this deliberately.

When a prompt leaves a bot to say what it has been doing, give it the facts
(`Lately_Text`: its run of kills, last quest, best thing it got, last ding),
or the model makes something vague up ("just finished some grinds"). Its
hooks run on map threads and only note names and numbers under a mutex. A
mingle premise that needs something real carries a tag (`[mount]`, `[pet]`,
`[loot]`, `[quest]`) and is only picked when it is there.

Name mounts, pets and companions as what they are, everywhere a prompt
mentions one: build the words with `Pets_Seen` and `TextRules::AnimalPhrases`
("riding their mount, a Swift Brown Horse", "with their hunter pet, a wolf
called Howler"). "with their Wolf Howler" beside talk of a mount had a bot
take a hunter's wolf for a mount.

Nothing from the game is made up. A prompt or premise that needs something
specific -- an item, quest, creature, boss, place -- hands the model the real
thing (`mod-ollama-chat_facts.cpp`'s `{trinket}`, `{itemset}`, `{rare}`,
`{bosses}`, `{class_races}`, or a conversation pool) rather than a category to
invent from: "a would-you-rather about trinkets" got "Trinket of the Hidden
Sun". `Outlook.FactsText` tells every bot to say it can't remember rather than
make an answer up; made-up fun is only fine as a plain imagine-if. The facts
pools are built from the server's own data on the world thread, on first use.
The same goes for replies: "need a wand" gets the bosses that really drop one
(`mod-ollama-chat_wants.cpp`, loot read from the world DB once), a quest is
described by its listed goals and its ender, never the quest log's sentence
("Kill 15 Blackrock Champions and Guard Howe in Lakeshire will reward you"
became "kill Guard Howe"), and a remark about a passer-by is handed something
real to remark on, with what is not there (`OllamaChatRequest::mustNotSay`:
mount words for someone on foot) dropped at delivery.

What chat itself is like is real too. When the talk is of chat, a reply is
told how busy General and Trade have really been (`Outlook_ChatFor`, counted
by `Context_ChannelLines`) and that every channel is in everyone's chat
window; a /say exchange between bots is told it every time
(`Mingle.ChatText`), being written all at once. Untold, bots on a realm
where nobody had typed in Trade all evening called it "basically a
warzone", and one said "brb gonna go check trade chat". A premise that puts
someone in a state they are not in is made up the same way: "one of them is
waiting for a group" was never true of a mingle, since a bot in a queue
never mingles.

Every line a bot says is in its own style. The model's lines are, through its
personality prompt; a typed (canned) line goes through
`Personality_StyleCanned(bot, line)` (`OllamaChat.Personality.CannedStyles`),
and a new place that says one must too. And only its own: a model copies the
chat above it, so one bot calling people "sugar" had half of General ending
every line with it. Pet names are their style's (`Response.PetNameStyles`,
once in a few lines) and taken out of everyone else's at delivery, and a
conversation winds down after a handful of lines (`Conversation.MaxLines`)
before it turns into one long running joke.

Every bot knows its side. `Outlook_ForBot` says which side it plays and its
own side's cities, and, standing in one of them, that it is safe and the
guards are its own (`Outlook_SideText`, `Outlook_HomeCity`: the zone's
capital flag and team, as the core decides hostile ground). A premise or pool
value about one side's places or people -- a city, a spot, a starting zone, a
quest, an NPC, a race, a race's mount -- carries `[horde]` or `[alliance]`,
read by `TextRules::TakeLevelAndSide` beside the level tag: untagged, a Horde
bot asked who was going AFK in Stormwind, and Horde bots in General argued
over whether draenei or gnomes dance better. The other side's races come up
only to be run down (`Pool.enemy_race`), and `{class_races}` is the bot's own
side's.

A place's guards and townsfolk are scenery (`OllamaIsScenery`), never a
topic or a line of the snapshot: told "about level 75" of an Orgrimmar
Grunt, a bot asked why the grunt was level 75. Decide it by what a creature
is, never by `IsFriendlyTo`: a city's people react to someone by that one's
standing with the city, and undead and blood elves start out only Neutral
with Orgrimmar.
Nor is anyone's level in a city (`OllamaInCity`): people of every level
are there, and "a level 12 Gnome Mage is near you" had a bot ask why
there was a level 12 there. A prompt about someone a bot sees there
leaves the level out (`Presence_Describe`, the nearby-player topic).

A race is more than a word in the prompt. What each race says comes from the
game -- its NPCs' greetings and farewells, its language's attested phrases
(`Race.Phrases.<Race>`, read by `mod-ollama-chat_race.cpp`) -- never made up:
check a new one against the game or the Warcraft wiki before adding it. A
bot is reminded of its own only on a small share of lines (`Race.UseChance`):
a list in every prompt is what every line would be about.

Keep the fourth wall out of prompts. As far as any bot knows, everyone on the
realm is a player like itself: write "players", "people", "someone", never
"bot" or "AI", and never single someone out as "a real player", which says the
rest are not ("the way a real player would", about the bot itself, is fine). A
model repeats what it is told, and `Response.FourthWallPhrases` then drops the
line. The one opening is a real player raising it (`Phrases_Check`'s
`answeringPlayer`).

## Conventions

- Log to `module.ollamachat`, not `server.loading`. It falls back to the
  `Logger.module` entry that ships in `worldserver.conf.dist`, so it works
  untouched and is separately tunable. Keep startup/registration messages on
  `server.loading`.
- Every new setting goes in `conf/mod_ollama_chat.conf.dist` with a comment
  block explaining what it does and its default. That file is the module's real
  documentation surface.
- Source files are globbed by AzerothCore (`modules/*/src/*.cpp`); new files
  need no CMake change.
- New tables go in `data/sql/characters/base/` and must be
  `CREATE TABLE IF NOT EXISTS`.
- `.ollama status` is a few lines (`OllamaStatus_Overview`): whether it is
  working, and an issue only for what someone should act on, with a threshold
  that keeps it quiet when all is well. A new count goes in a part of
  `OllamaStatus_Lines` (`chat`, `talk`, `city`, `party`, `bots`), which the
  full report (`mod-ollama-chat_report.cpp`, `Report.File`) writes out whole;
  never in the overview.
- A module's own log file is set up in worldserver.conf, not its module conf:
  the core sets the logs up before it reads module confs, so an
  `Appender.*`/`Logger.*` there does nothing until `.reload config`.

## Checking your work without a full build

A full AzerothCore build is slow. For a syntax + semantic check of just this
module, `cl /Zs` against the collected include paths works and catches
signature mismatches, missing declarations and bad `override`s:

- include dirs: every subdir of `src/common` and `src/server`, every deps dir
  containing headers **plus its parent** (fmt needs `deps/fmt/include`),
  all of `modules/mod-playerbots/src`, this module's `src` and `deps`, plus
  boost, the MySQL include dir and `var/build/obj` for `revision.h`
- exclude `deps/g3dlite/include/G3D` — it has `Log.h`, `Random.h` and
  `Spline.h` that shadow AzerothCore's
- exclude `deps/jemalloc` — its internal `Util.h` shadows AzerothCore's
- needs `/std:c++20 /utf-8 /FI"Log.h"` (several core headers assume the PCH has
  already pulled `Log.h` in)
- pass the flags in a response file; ~490 include paths overflow the command
  line

This does not link, so it will not catch a declared-but-undefined function.
