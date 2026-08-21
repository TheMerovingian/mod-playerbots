-- Gather-zone occupancy tracker for the 'gather resources' downtime behaviour.
-- One row per active bot currently farming a profession resource zone, so bots
-- can read a zone's active count from the database instead of scanning every
-- other bot each tick. Cleared wholesale on server startup (stale rows would
-- otherwise inflate counts after a crash / logout).
DROP TABLE IF EXISTS `playerbots_gather_reservation`;
CREATE TABLE `playerbots_gather_reservation` (
    `guid` int unsigned NOT NULL COMMENT 'Bot character low GUID (unique: one active run per bot)',
    `skill_id` int unsigned NOT NULL COMMENT 'SKILL_MINING / SKILL_HERBALISM / SKILL_SKINNING',
    `zone_id` int unsigned NOT NULL COMMENT 'Area id of the zone being farmed',
    `map_id` int unsigned NOT NULL COMMENT 'World map id of the reserved zone',
    PRIMARY KEY (`guid`),
    KEY `idx_skill_zone` (`skill_id`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot gather zone reservations';