# Error code reference

| Status | Meaning |
|---|---|
| SUCCESS | call completed fully |
| PARTIAL_RESULT | valid result contains an unknown/failed item |
| INVALID_ARGUMENT | invalid pointer, header, enum, option, or bound |
| UNSUPPORTED | capability or topology is intentionally unsupported |
| NOT_INITIALIZED | object or Core is not serving |
| UNAVAILABLE | daemon, Provider, PDRL, Device, or snapshot unavailable |
| PERMISSION_DENIED | caller lacks permission |
| TIMEOUT | bounded deadline expired |
| RESOURCE_EXHAUSTED | configured capacity was reached |
| BUFFER_TOO_SMALL | caller storage is insufficient; required count is set |
| NOT_FOUND | current entity or item does not exist |
| STALE_GENERATION | reference belongs to an old entity/catalog generation |
| INTERNAL | invariant, serialization, or peer-contract failure |
| PROTOCOL_INCOMPATIBLE | client and daemon protocol versions cannot interoperate |

Call-level status does not replace item status. Partial results preserve valid
items. Unsupported or unavailable values are never represented by a fabricated
zero.
