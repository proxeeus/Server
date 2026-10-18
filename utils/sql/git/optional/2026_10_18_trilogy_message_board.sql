-- ============================================================================
-- Trilogy: message board storage
-- ============================================================================
--
-- The bulletin boards in city inns (door opentype 55, model BBBOARD) open a
-- v29c window with six categories, a list, a reader and a compose screen.  The
-- client does all of the UI itself; the server only stores posts and answers
-- four requests (0x0821 list, 0x0921 read, 0x0c21 post, 0x0d21 erase).  See the
-- opcode block for ZN_OP_MBRetrieveMessages in zone/trilogy_zone.cpp.
--
-- One table for the whole server, as in EQClassic's mb_messages: every board in
-- every zone shows the same posts.  `id` is global rather than EQClassic's
-- per-category MAX(id)+1 (that read-then-insert races between zone processes);
-- the client only ever echoes it back, so nothing depends on it being small.
--
--   category  0 Official (GM-only to post), 1 For Sale, 2 General,
--             3 Help Wanted, 4 Personals, 5 Guilds
--   date      the poster's client stamp, Norrath game time as YYMMDDHHMM;
--             the list renders it as MM/DD/YY HH:MM
--   author    character name at the time of posting (what the client compares
--             against its own name to decide whether to offer Erase)
--   char_id   who may erase it, independent of renames
--   language  language id the post was written in; readers see it garbled by
--             their own skill in that language
--   subject   at most 29 characters (client input limit)
--   message   at most 1023 characters and 10 lines (client input limit)
--   time      unix time of the post, newest first in the list
--
-- Creates a table only; touches nothing that exists.  Safe to re-run.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `trilogy_message_board` (
  `id`       int(10) unsigned    NOT NULL AUTO_INCREMENT,
  `category` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `date`     varchar(16)         NOT NULL DEFAULT '',
  `author`   varchar(64)         NOT NULL DEFAULT '',
  `char_id`  int(10) unsigned    NOT NULL DEFAULT 0,
  `language` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `subject`  varchar(32)         NOT NULL DEFAULT '',
  `message`  text                NOT NULL,
  `time`     int(10) unsigned    NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `category_time` (`category`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;
