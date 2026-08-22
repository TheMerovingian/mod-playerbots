-- Gathering-node (mining / herbalism) index used by the 'gather resources'
-- behaviour. One row per (zone, gameobject, skill) recording that a harvestable
-- node spawns in the zone and requires the given gathering skill at the given
-- tier (Lock.dbc required skill value). Rebuilt on every server start from
-- gameobject spawn / template / lock data, and queried at runtime instead of
-- scanning the spawn tables per session:
--   * what tiers exist for a skill (for highest-vs-random tier selection),
--   * which zones hold a given skill+tier,
--   * the map / position of the representative spawn used as the travel point.
DROP TABLE IF EXISTS `playerbots_node`;
CREATE TABLE `playerbots_node` (
    `id` int unsigned NOT NULL AUTO_INCREMENT,
    `skill_id` int unsigned NOT NULL COMMENT 'SKILL_MINING / SKILL_HERBALISM',
    `zone_id` int unsigned NOT NULL COMMENT 'Area id where the node spawns',
    `tier` int unsigned NOT NULL DEFAULT 0 COMMENT 'Required skill value (Lock.dbc) for this node',
    `map_id` int unsigned NOT NULL DEFAULT 0 COMMENT 'World map id of the representative spawn',
    `pos_x` float NOT NULL DEFAULT 0 COMMENT 'Representative spawn coordinate (travel point)',
    `pos_y` float NOT NULL DEFAULT 0,
    `pos_z` float NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uidx_skill_zone_tier` (`skill_id`, `zone_id`, `tier`),
    KEY `idx_skill_tier` (`skill_id`, `tier`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot gathering node index by skill/zone/tier';
