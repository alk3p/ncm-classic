CREATE TABLE IF NOT EXISTS track(
	file TEXT,
	tid TEXT,
	aid TEXT,
	dir TEXT,
	title TEXT,
	album TEXT,
	genre TEXT,
	artist TEXT,
	duration REAL,
	timestamp INTEGER,
	bitrate INTEGER,
	filesize INTEGER,
	ignored INTEGER DEFAULT 0,
	id TEXT,
	artistid TEXT DEFAULT "",
	parentdir TEXT DEFAULT "",
	track TEXT,
	librarypath TEXT DEFAULT "",
	tracknumber INTEGER,
	source TEXT DEFAULT "",
	starttime REAL DEFAULT 0,
	type INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS track_library ON track(dir, id);
