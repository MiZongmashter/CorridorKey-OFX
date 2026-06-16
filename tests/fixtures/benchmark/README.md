# Benchmark Fixture Media

Benchmark media is intentionally not committed. Operators may place local media
in this directory, but raw plates, alpha hints, project names, and complete
local media paths must not be committed.

## Required Local Media Classes

Provide one source plate plus one matching alpha hint for each class below when the hardware and host row are available:

| Class | Expected source | Expected alpha hint | Resolution target | Required notes |
| --- | --- | --- | --- | --- |
| `hd` | `hd/source` | `hd/alpha_hint` | 1920x1080 or nearest HD source | green and/or blue screen color, input color mode, bit depth |
| `4k` | `4k/source` | `4k/alpha_hint` | 3840x2160 or DCI 4K equivalent | same frame range and matte semantics as HD where practical |
| `8k` | `8k/source` | `8k/alpha_hint` | 7680x4320 or target 8K equivalent | may be omitted when target hardware cannot safely run it |

## Optional Manifest

A local `manifest.json` may be created next to this README. Keep it untracked.
Use opaque media IDs rather than raw paths in copied summaries. If local
absolute paths are necessary for benchmarking, they must remain outside the
public repository.

Suggested shape:

```json
{
  "media": [
    {
      "id": "private-hd-green-001",
      "class": "hd",
      "source": "hd/source.ckfb",
      "alpha_hint": "hd/alpha_hint.ckfb",
      "screen_color": "green",
      "quality": "high_1024",
      "privacy": "private-local-do-not-commit"
    }
  ]
}
```

Benchmark scripts must not silently substitute private media or claim HD/4K/8K
release performance from synthetic placeholders.
