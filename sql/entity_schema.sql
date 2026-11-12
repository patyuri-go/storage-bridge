-- entity 库表结构（PostgreSQL 13+，gen_random_uuid() 已在内核里，无需 pgcrypto）
-- psql "host=192.168.10.221 port=5432 dbname=entity user=postgres" -f sql/entity_schema.sql

CREATE TABLE IF NOT EXISTS entities (
    entity_id   UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    sha256      CHAR(64)    NOT NULL,
    size        BIGINT      NOT NULL CHECK (size >= 0),
    mime_type   TEXT        NOT NULL DEFAULT 'application/octet-stream',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT entities_sha256_hex CHECK (sha256 ~ '^[0-9a-f]{64}$')
);

-- 同一内容只应有一个实体（去重的核心约束）
CREATE UNIQUE INDEX IF NOT EXISTS entities_sha256_key ON entities (sha256);

CREATE TABLE IF NOT EXISTS storage_nodes (
    storage_id  UUID        PRIMARY KEY,
    name        TEXT        NOT NULL,
    type        TEXT        NOT NULL CHECK (type IN ('s3', 'webdav', 'local')),
    endpoint    TEXT        NOT NULL,
    config      JSONB       NOT NULL DEFAULT '{}'::jsonb,
    state       TEXT        NOT NULL DEFAULT 'active'
                            CHECK (state IN ('active', 'disabled')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT storage_nodes_name_key UNIQUE (name)
);

CREATE TABLE IF NOT EXISTS entity_locations (
    entity_id   UUID        NOT NULL REFERENCES entities (entity_id) ON DELETE CASCADE,
    storage_id  UUID        NOT NULL REFERENCES storage_nodes (storage_id) ON DELETE CASCADE,
    key         TEXT        NOT NULL,
    state       TEXT        NOT NULL DEFAULT 'unverified'
                            CHECK (state IN ('available', 'missing', 'corrupted', 'unverified')),
    verified_at TIMESTAMPTZ,
    failed_at   TIMESTAMPTZ,
    PRIMARY KEY (entity_id, storage_id, key)
);

-- 一个存储节点上同一个 key 只对应一个实体
CREATE UNIQUE INDEX IF NOT EXISTS entity_locations_storage_key
    ON entity_locations (storage_id, key);

-- 按 key 前缀（目录）查、按节点查失败项
CREATE INDEX IF NOT EXISTS entity_locations_key_prefix
    ON entity_locations (key text_pattern_ops);
CREATE INDEX IF NOT EXISTS entity_locations_state
    ON entity_locations (storage_id, state);
