-- The journal of mod-ollama-chat (mod-ollama-chat_journal): what a bot and
-- someone playing have done together, written down from the game as it
-- happens and known the next time they meet.
--
-- One row is one thing, or one outing's worth of it: a dungeon they were in
-- (its bosses in `detail`, its wipes in `wipes`), a zone they quested in
-- (`times` quests), a duel, something sold, bought or given, a resurrection,
-- a battleground fought on the same team (`times` matches, the ones won in
-- `detail`, the ones lost in `wipes`). A pair keeps a few rows
-- (OllamaChat.Journal.KeepPerPair) for OllamaChat.Journal.KeepDays; the module
-- rewrites a pair's rows together, and makes the table itself at startup when
-- it is not there.

CREATE TABLE IF NOT EXISTS mod_ollama_chat_journal (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    bot_guid BIGINT UNSIGNED NOT NULL,
    player_guid BIGINT UNSIGNED NOT NULL,
    kind TINYINT UNSIGNED NOT NULL COMMENT '1 dungeon, 2 questing, 3 duel, 4 sold, 5 bought, 6 gave, 7 given, 8 raised, 9 battleground',
    subject VARCHAR(120) NOT NULL DEFAULT '' COMMENT 'The dungeon, the zone, the item, where, the battleground',
    detail VARCHAR(400) NOT NULL DEFAULT '' COMMENT 'A run''s bosses, a sale''s price, who won a duel, matches won',
    times INT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'Bosses down, quests done, duels fought, matches played',
    wipes INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'A run''s wipes, a battleground''s matches lost',
    first_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_pair (bot_guid, player_guid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
