CREATE TABLE games (
    id TEXT PRIMARY KEY NOT NULL,
    root_fen TEXT NOT NULL,
    final_fen TEXT NOT NULL,
    moves TEXT NOT NULL,
    human_color TEXT NOT NULL CHECK (human_color IN ('white', 'black')),
    depth INTEGER NOT NULL CHECK (depth BETWEEN 1 AND 100),
    turn TEXT NOT NULL CHECK (turn IN ('white', 'black')),
    status TEXT NOT NULL CHECK (status IN ('active', 'completed')),
    result TEXT NOT NULL CHECK (result IN ('*', '1-0', '0-1', '1/2-1/2')),
    termination TEXT NOT NULL CHECK (
        termination IN (
            'ongoing', 'checkmate', 'stalemate', 'threefold', 'fifty-move',
            'resignation'
        )
    ),
    white TEXT NOT NULL,
    black TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    revision INTEGER NOT NULL DEFAULT 0 CHECK (revision >= 0),
    CHECK (json_valid(moves)),
    CHECK (json_type(moves) = 'array')
);

CREATE INDEX games_updated ON games (updated_at DESC, id DESC);
