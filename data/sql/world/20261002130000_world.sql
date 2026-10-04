-- Quest-hub table for the travel layer.
--
-- A hub is a cluster of quest givers - what a player calls a town or quest camp.
-- The travel layer uses it to prefer destinations that sit in a hub with work for the
-- bot's level band over lone quest givers in the middle of nowhere, and to keep a bot
-- working in the hub it already travelled to.
--
-- The rows are generated off-instance by tools/build-quest-hubs.py in the deployment
-- repo (it clusters creature_questrelation spawns) and applied through custom-sql, so
-- this migration only owns the schema. An empty table is a supported state: the layer
-- logs it once and behaves exactly as before.

CREATE TABLE IF NOT EXISTS `ai_playerbot_questhub` (
  `id` mediumint unsigned NOT NULL,
  `map` mediumint unsigned NOT NULL,
  `x` float NOT NULL,
  `y` float NOT NULL,
  `givers` smallint unsigned NOT NULL DEFAULT 0,
  `quests` smallint unsigned NOT NULL DEFAULT 0,
  `level_min` tinyint unsigned NOT NULL DEFAULT 0,
  `level_median` tinyint unsigned NOT NULL DEFAULT 0,
  `level_max` tinyint unsigned NOT NULL DEFAULT 0,
  `q01_05` smallint unsigned NOT NULL DEFAULT 0,
  `q06_10` smallint unsigned NOT NULL DEFAULT 0,
  `q11_15` smallint unsigned NOT NULL DEFAULT 0,
  `q16_20` smallint unsigned NOT NULL DEFAULT 0,
  `q21_25` smallint unsigned NOT NULL DEFAULT 0,
  `q26_30` smallint unsigned NOT NULL DEFAULT 0,
  `q31_35` smallint unsigned NOT NULL DEFAULT 0,
  `q36_40` smallint unsigned NOT NULL DEFAULT 0,
  `q41_45` smallint unsigned NOT NULL DEFAULT 0,
  `q46_50` smallint unsigned NOT NULL DEFAULT 0,
  `q51_55` smallint unsigned NOT NULL DEFAULT 0,
  `q56_60` smallint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `map` (`map`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
