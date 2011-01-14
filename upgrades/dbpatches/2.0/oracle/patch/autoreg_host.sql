DROP INDEX autoreg_host_1;
CREATE INDEX autoreg_host_1 ON autoreg_host (proxy_hostid,host);
ALTER TABLE autoreg_host MODIFY autoreg_hostid DEFAULT NULL;
ALTER TABLE autoreg_host MODIFY proxy_hostid DEFAULT NULL;
ALTER TABLE autoreg_host MODIFY proxy_hostid NULL;
ALTER TABLE autoreg_host ADD listen_ip nvarchar2(39) DEFAULT '';
ALTER TABLE autoreg_host ADD listen_port number(10) DEFAULT '0' NOT NULL;
UPDATE autoreg_host SET proxy_hostid=NULL WHERE proxy_hostid=0;
DELETE FROM autoreg_host WHERE proxy_hostid IS NOT NULL AND proxy_hostid NOT IN (SELECT hostid FROM hosts);
ALTER TABLE autoreg_host ADD CONSTRAINT c_autoreg_host_1 FOREIGN KEY (proxy_hostid) REFERENCES hosts (hostid) ON DELETE CASCADE;
