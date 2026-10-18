# verbosity

**Protocols:** memcached text · memcached binary (`0x1b`)

Accepted as a no-op. pace-kv's logging is configured through the
`ILogger` interface, not the memcached verbosity dial.

## Synopsis

```text
verbosity <level> [noreply]\r\n
```

## Response

```text
OK\r\n
```

The level value is parsed but discarded.
