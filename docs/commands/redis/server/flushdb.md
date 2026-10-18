# FLUSHDB

**Protocols:** Redis RESP2

Removes every entry. pace-kv has a single keyspace, so this is
identical to [FLUSHALL](flushall.md).

## Synopsis

```text
FLUSHDB [ASYNC | SYNC]
```

## Response

```text
+OK
```
