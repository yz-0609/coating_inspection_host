-- 初始结构；由 CMake 嵌入程序，运行时不依赖外部 SQL 文件。
CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);
CREATE TABLE inspection(
 inspection_id TEXT PRIMARY KEY,workpiece_id TEXT NOT NULL,product_id TEXT NOT NULL,
 session_token INTEGER NOT NULL,state TEXT NOT NULL CHECK(state IN('START_PENDING','INSPECTING','COMPLETED','ABORTED')),
 result TEXT CHECK(result IN('PASS','FAIL') OR result IS NULL),required_points INTEGER NOT NULL CHECK(required_points BETWEEN 1 AND 5),
 rule_version TEXT NOT NULL,rule_snapshot TEXT NOT NULL,started_at_ms INTEGER NOT NULL,finished_at_ms INTEGER,
 abort_reason TEXT,time_quality TEXT NOT NULL,run_id TEXT NOT NULL);
CREATE TABLE measurement(
 inspection_id TEXT NOT NULL REFERENCES inspection(inspection_id),point_index INTEGER NOT NULL CHECK(point_index BETWEEN 1 AND 5),
 candidate_id TEXT NOT NULL,thickness_milli_um INTEGER NOT NULL,substrate_mode TEXT NOT NULL,instrument_id TEXT NOT NULL,
 group_name TEXT NOT NULL,connection_generation INTEGER NOT NULL,local_sequence INTEGER NOT NULL,
 received_at_ms INTEGER NOT NULL,received_mono_ms INTEGER NOT NULL,confirmed_at_ms INTEGER NOT NULL,
 PRIMARY KEY(inspection_id,point_index),UNIQUE(inspection_id,candidate_id));
CREATE TABLE outbox(
 event_id TEXT PRIMARY KEY,inspection_id TEXT NOT NULL REFERENCES inspection(inspection_id),event_type TEXT NOT NULL,
 payload TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'PENDING',attempt_count INTEGER NOT NULL DEFAULT 0,
 request_id TEXT,last_error TEXT,created_at_ms INTEGER NOT NULL,accepted_at_ms INTEGER,
 UNIQUE(inspection_id,event_type));
CREATE TABLE actuation_log(
 id INTEGER PRIMARY KEY,inspection_id TEXT REFERENCES inspection(inspection_id),host_epoch INTEGER NOT NULL,
 command_seq INTEGER NOT NULL,command TEXT NOT NULL,command_payload TEXT NOT NULL,status TEXT NOT NULL,
 acknowledged_at_ms INTEGER,detail TEXT,UNIQUE(host_epoch,command_seq));
CREATE INDEX workpiece_lookup ON inspection(workpiece_id,started_at_ms);
CREATE INDEX pending_lookup ON outbox(state,created_at_ms);
PRAGMA user_version=1;
