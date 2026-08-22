-- Auction house pricing + sale history tracking for playerbots.
-- playerbots_auction_pricing: rolling single-row aggregate used to price each listing.
DROP TABLE IF EXISTS `playerbots_auction_pricing`;
CREATE TABLE `playerbots_auction_pricing` (
    `item_entry` int unsigned NOT NULL COMMENT 'Item template entry',
    `last_sold_price` int unsigned NOT NULL DEFAULT 0 COMMENT 'Per-unit sale price of the most recent completed sale (copper)',
    `last_sold_at` int unsigned NOT NULL DEFAULT 0 COMMENT 'Unix time of the most recent completed sale',
    `last_listed_at` int unsigned NOT NULL DEFAULT 0 COMMENT 'Unix time the most recent listing was created',
    `sale_count` int unsigned NOT NULL DEFAULT 0 COMMENT 'Cumulative number of completed sales',
    PRIMARY KEY (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot auction pricing aggregates';

-- playerbots_auction_sale_history: one row per listing, finalised when the auction sells.
DROP TABLE IF EXISTS `playerbots_auction_sale_history`;
CREATE TABLE `playerbots_auction_sale_history` (
    `id` int unsigned NOT NULL AUTO_INCREMENT,
    `auction_id` int unsigned NOT NULL COMMENT 'Core auctionentry.Id',
    `player_id` int unsigned NOT NULL COMMENT 'Seller character low GUID',
    `item_id` int unsigned NOT NULL COMMENT 'Item template entry',
    `stack_size` int unsigned NOT NULL DEFAULT 0 COMMENT 'Stack put up for auction; 0 if the item is not stackable',
    `listing_price` int unsigned NOT NULL DEFAULT 0 COMMENT 'Start bid (copper)',
    `buyout_price` int unsigned NOT NULL DEFAULT 0 COMMENT 'Buyout (copper)',
    `sale_price` int unsigned NOT NULL DEFAULT 0 COMMENT 'Final sale price (copper); 0 while unsold',
    `time_listed` int unsigned NOT NULL DEFAULT 0 COMMENT 'Unix time the listing was created',
    `time_sold` int unsigned NOT NULL DEFAULT 0 COMMENT 'Unix time the auction sold; 0 while unsold',
    PRIMARY KEY (`id`),
    KEY `idx_auction_id` (`auction_id`),
    KEY `idx_item_id` (`item_id`),
    KEY `idx_time_sold` (`time_sold`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot auction sale history';