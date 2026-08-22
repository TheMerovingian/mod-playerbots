-- Skinnable monster index used by the 'gather resources' skinning behaviour.
-- One row per (zone, monster, leather): it records that the skinnable monster
-- entry spawns in the zone and yields the given leather (identified by its skin
-- loot table id). The table is rebuilt on every server start from the creature /
-- spawn data, so it also serves as the runtime lookup for:
--   * what zones hold a given leather type (idx on leather),
--   * which monster to target in a zone after the bot arrives there.
-- `leather_id` is the creature's skin loot table id (CreatureTemplate.SkinLootId),
-- i.e. the drop set that produces the leather. `tier` is the effective skin tier
-- (derived from the monster level, matching the level->skill formula). `map_id`
-- and the position columns are the representative spawn used as the travel point.
DROP TABLE IF EXISTS `playerbots_skin`;
CREATE TABLE `playerbots_skin` (
    `id` int unsigned NOT NULL AUTO_INCREMENT,
    `leather_id` int unsigned NOT NULL COMMENT 'Skin loot table id (creature.SkinLootId) yielding the leather',
    `zone_id` int unsigned NOT NULL COMMENT 'Area id where the monster spawns',
    `monster_id` int unsigned NOT NULL COMMENT 'Creature template entry of the skinnable monster',
    `tier` int unsigned NOT NULL DEFAULT 0 COMMENT 'Effective skin tier (from monster level) for highest/random selection',
    `map_id` int unsigned NOT NULL DEFAULT 0 COMMENT 'World map id of the representative spawn',
    `pos_x` float NOT NULL DEFAULT 0 COMMENT 'Representative spawn coordinate (travel point)',
    `pos_y` float NOT NULL DEFAULT 0,
    `pos_z` float NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uidx_zone_monster_leather` (`zone_id`, `monster_id`, `leather_id`),
    KEY `idx_leather_zone` (`leather_id`, `zone_id`),
    KEY `idx_zone` (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot skinnable monster index by zone/leather';