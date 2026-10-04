-- Per-quest profile table for the bot planner.
--
-- The travel layer ranks destinations by distance, so a quest that matters (a class
-- chain, something that teaches a spell, a long chain with content behind it) is ranked
-- exactly like filler. These columns are the "why would a player bother" inputs a
-- planner can read without understanding the quest text:
--
--   kind               unlock  - teaches a spell, or limited by class/skill (permanent)
--                      chain   - has follow-up quests (guidance chain)
--                      reward  - choice reward or a rare-or-better reward item
--                      filler  - everything else
--   chain_reach        distinct quests reachable by following PrevQuestId / NextQuestId
--                      / NextQuestInChain links (an approximation of "content behind it")
--   best_reward_*      best quality / item level among the quest's reward items
--
-- The rows are generated off-instance by tools/build-quest-profile.py in the deployment
-- repo and applied through custom-sql, so this migration only owns the schema. An empty
-- table is a supported state: nothing reads it yet, the planner work comes next.

CREATE TABLE IF NOT EXISTS `ai_playerbot_quest_profile` (
  `entry` mediumint unsigned NOT NULL,
  `title` varchar(200) NOT NULL DEFAULT '',
  `quest_level` tinyint unsigned NOT NULL DEFAULT 0,
  `min_level` tinyint unsigned NOT NULL DEFAULT 0,
  `qtype` tinyint unsigned NOT NULL DEFAULT 0,
  `required_classes` mediumint unsigned NOT NULL DEFAULT 0,
  `required_skill` smallint unsigned NOT NULL DEFAULT 0,
  -- Signed on purpose: PrevQuestId is negative for "available only if that quest is
  -- NOT taken" (e.g. -253). Only positive values are chain edges.
  `prev_quest` mediumint NOT NULL DEFAULT 0,
  `next_quest` mediumint NOT NULL DEFAULT 0,
  `next_in_chain` mediumint NOT NULL DEFAULT 0,
  `chain_reach` smallint unsigned NOT NULL DEFAULT 0,
  `teaches_spell` tinyint unsigned NOT NULL DEFAULT 0,
  `choice_rewards` tinyint unsigned NOT NULL DEFAULT 0,
  `best_reward_quality` tinyint unsigned NOT NULL DEFAULT 0,
  `best_reward_ilvl` smallint unsigned NOT NULL DEFAULT 0,
  `reward_xp` mediumint unsigned NOT NULL DEFAULT 0,
  `reward_money` int NOT NULL DEFAULT 0,
  `kind` enum('unlock','chain','reward','filler') NOT NULL DEFAULT 'filler',
  PRIMARY KEY (`entry`),
  KEY `kind` (`kind`),
  KEY `quest_level` (`quest_level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
