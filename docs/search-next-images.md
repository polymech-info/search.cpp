# Image Search Next

This note narrows the unified search plan for images. The goal is to turn image
files from path-only hits into searchable units with stable locators:
image file, OCR page/block/line/word, layout region, caption, visual metadata,
EXIF, and optional bounding boxes.

Main search design remains in `docs/search/search-next.md`.

## Current Starting Point

- `search --content image` is currently exact path/name oriented in the unified
  CLI plan. The first vector-indexing slice now has `image-sidecar`, which emits
  file/path/probe/sidecar metadata units without OCR or LLM inference.
- Older image find lives around `src/core/find.hpp` / `src/core/find.cpp` and can
  search sidecar metadata, EXIF, filenames, and optional LLM judgement.
- Old sidecar metadata generation is exposed by `src/cli/pm_image_cmd_meta.cpp`
  and calls `media::meta_extract`.
- Newer agent/path tooling has side-effect-free image understanding in
  `src/llm/path_tool_executor.hpp` and `src/llm/tools/pictures/PictureTools.cpp`.
  `image_understand` resolves provider/model from the Chat image recognition
  preset.
- OCR building blocks already exist in `src/xblox/blocks/ocr_blocks.cpp`:
  PP-DocLayoutV3 layout detection, local OCR VLM, table/text/layout modes,
  markdown output, model discovery, and thread/backend controls. We are not
  keeping raw PaddleOCR ONNX text recognition for search ingest; its quality was
  not reliable enough.
- Test coverage already exercises OCR/VL flows through commands such as:

```powershell
npm run test:ocr:vl:layout:threads
```

## Target Layout

```txt
src/search/
  source_adapter.hpp
  source_adapter.cpp                 # text and thin shared adapters
  source_adapter_code.cpp            # CodeSourceAdapter
  source_adapter_image.cpp           # Image adapters and shared image helpers
  source_adapter_registry.cpp
  fingerprint.cpp
  vector_backend.cpp

src/lib/llm/
  image_ocr_adapter.hpp/.cpp         # optional lower-level OCR/local wrapper
  image_meta_adapter.hpp/.cpp        # optional lower-level LLM metadata wrapper

src/llm/tools/pictures/
  PictureTools.cpp                   # existing image_understand / ocr_text path tools

src/xblox/blocks/
  ocr_blocks.cpp                     # existing OCR runtime implementation reference
```

Adapter ids:

- `ocr-local`
  local OCR/layout/table extraction. Current implementation uses
  PP-DocLayoutV3 only for layout/table detection, then runs the preferred OCR
  VLM, favoring PaddleOCR-VL from `local_model_catalog`.
- `llm-meta`
  general image caption/metadata extraction through the same Chat image
  recognition provider/model route used by `image_understand`. Current
  implementation calls that route directly during explicit ingest/detail and
  does not require pre-existing sidecar files.
- `image-sidecar`
  cheap adapter for existing `<stem>.json`, `<stem>.md`, `<stem>.txt`,
  `<image>.json`, `<image>.md`, and probe/path metadata without model inference.

## Adapter Shape

Image adapters should emit `SourceUnit` records into the same pipeline as text,
Office, code, and future PDF units.

Image file unit:

```json
{
  "uri": "photos/invoice.jpg#image",
  "path": "photos/invoice.jpg",
  "type": "image-file",
  "title": "invoice.jpg",
  "text": "invoice.jpg photo image receipt document",
  "facts": {
    "adapter": "image-sidecar",
    "extension": ".jpg",
    "size": 248901,
    "image": {
      "width": 1600,
      "height": 1200
    },
    "sourcePath": {}
  },
  "locator": {
    "kind": "image",
    "path": "photos/invoice.jpg"
  }
}
```

OCR text unit:

```json
{
  "uri": "photos/invoice.jpg#ocr:block:3",
  "path": "photos/invoice.jpg",
  "type": "image-ocr-block",
  "title": "OCR block 3",
  "text": "Rosa overdue A240352 224.72",
  "facts": {
    "adapter": "ocr-local",
    "ocrMode": "layout",
    "provider": "layout",
    "model": "PP-DocLayoutV3 + PaddleOCR-VL",
    "confidence": 0.92,
    "layoutLabel": "text"
  },
  "locator": {
    "kind": "bbox",
    "unit": "ocr-block",
    "page": 1,
    "x": 124,
    "y": 520,
    "w": 880,
    "h": 96
  }
}
```

LLM metadata unit:

```json
{
  "uri": "photos/product.jpg#meta:llm",
  "path": "photos/product.jpg",
  "type": "image-meta",
  "title": "product.jpg visual description",
  "text": "Red product photo on a white background with front-facing packaging.",
  "facts": {
    "adapter": "llm-meta",
    "provider": "google",
    "model": "chat-preset-image-recognition",
    "labels": ["product", "red", "packaging"],
    "objects": ["box", "label"],
    "colors": ["red", "white"]
  },
  "locator": {
    "kind": "image-meta",
    "path": "photos/product.jpg"
  }
}
```

## Units To Extract

First slice:

- `image-file`
  path, size, extension, image dimensions, source path facts.
- `image-ocr-page`
  compact searchable OCR text per image/page; current `ocr-local` slice emits
  one non-chunked vector unit so initial semantic search returns one hit per
  image-derived unit. Full text is pulled through detail when needed.
- `image-ocr-block`
  layout/text/table blocks with confidence and bounding box; planned after the
  page-level unit is stable.
- `image-ocr-line`
  line-level OCR text when available.
- `image-meta`
  caption, summary, labels, objects, colors, scene/category guesses. Current
  `llm-meta` slice emits one compact, non-chunked metadata unit from
  `image_understand`.

Later:

- `image-ocr-word`
  word-level facts for precise highlighting and detail.
- `image-layout-region`
  table/figure/title/list/formula/layout labels independent from OCR text.
- `image-object`
  detected objects with bounding boxes once a detector/classifier backend is
  selected.
- `image-reference-match`
  similarity to one or more reference images.

## Locators

Every image-derived unit should include adapter-owned detail locators:

```json
{
  "kind": "bbox",
  "adapter": "ocr-local",
  "page": 1,
  "x": 124,
  "y": 520,
  "w": 880,
  "h": 96,
  "confidence": 0.92,
  "label": "text"
}
```

`search detail` can then expand:

- full image metadata;
- OCR block plus nearby blocks by read order;
- line/word details;
- layout/table markdown;
- crop path or generated crop bytes later;
- the exact model/provider/options used to create the unit.

## Fingerprints

Cheap fingerprints should work without model inference when sidecars already
exist. Model-backed adapters can enrich the same sidecar after an explicit
`search index`.

```json
{
  "typeGuess": "image",
  "keywords": ["invoice", "rosa", "overdue", "receipt"],
  "summary": "Scanned invoice or billing screenshot.",
  "structure": {
    "ocr": {
      "blocks": 5,
      "hasTable": true
    },
    "labels": ["document", "invoice"],
    "objects": []
  },
  "facts": {
    "adapter": "ocr-local",
    "sourcePath": {},
    "image": {
      "width": 1600,
      "height": 1200
    }
  }
}
```

`--mode fingerprint` remains the fast LLM/tool routing path:

```powershell
tanit-cli search search ./photos --content image --mode fingerprint --query "invoice rosa overdue" --json
```

If a hit is promising, the LLM/tool can ask for indexed detail from the same
fast lookup. This reads the stored vector document text and locator, and should
not re-run OCR/LLM extraction for obvious indexed hits:

```powershell
tanit-cli search search --adapter ocr-local --path ./photos/*.png --fingerprint cheap --include-detail page --query "invoice rosa overdue" --json
```

Use explicit semantic search or `search detail` when the indexed text is not
enough:

```powershell
tanit-cli search search ./photos --content image --mode semantic --backend vector --query "invoice with overdue rosa amount" --json
tanit-cli search detail --adapter ocr-local --path ./photos/invoice.jpg --selector 'block:3' --include-detail block --context-before 2 --context-after 2 --json
```

## CLI Args

Use the same `search index/search/detail` shape, with image-specific adapter
options carried as regular CLI args.

```powershell
# Build OCR-backed image index.
tanit-cli search index ./photos --content image --backend vector --adapter ocr-local --index-policy same-dir --fingerprint cheap --ocr-mode layout --ocr-provider auto --ocr-backend auto --ocr-threads 8 --model bge-small-en-v1.5-q8_0.gguf --gpu-layers -1 --log-level trace --json

# Build general visual metadata index through Chat image recognition settings.
tanit-cli search index ./photos --content image --backend vector --adapter llm-meta --index-policy same-dir --fingerprint cheap --meta-prompt "Describe searchable visual metadata, objects, scene, text, colors, and labels." --resize-width 768 --log-level trace --json

# Inspect one image through the same route without writing sidecars.
tanit-cli search detail --adapter llm-meta --path ./photos/product.jpg --include-detail metadata --meta-prompt "Describe this image for search indexing in one short paragraph." --json

# Fast image search after indexing. `--adapter image-sidecar` implies image
# content; `--fingerprint cheap` implies fingerprint mode when --mode is omitted.
# Inline detail comes from the index for obvious hits.
tanit-cli search search --adapter image-sidecar --path ./photos --fingerprint cheap --include-detail metadata --query "red product photo" --json

# Semantic image metadata/OCR search after indexing.
tanit-cli search search ./photos --content image --mode semantic --backend vector --no-index-create --query "document table with invoice total" --json

# OCR/layout detail expansion.
tanit-cli search detail --adapter ocr-local --path ./photos/invoice.jpg --selector 'bbox:124,520,880,96' --include-detail block --context-before 1 --context-after 2 --json

# LLM metadata detail expansion.
tanit-cli search detail --adapter llm-meta --path ./photos/product.jpg --include-detail metadata --json
```

Proposed image-specific flags:

- `--adapter ocr-local|llm-meta|image-sidecar|auto`
  selects which image adapter should extract units during indexing.
- `--ocr-mode layout|table|text`
  mirrors `ocrText` mode.
- `--ocr-provider auto|layout|table|text|vlm|paddle`
  mirrors the XBlox OCR provider choices.
- `--ocr-backend auto|cpu|gpu|cuda|coreml|metal`
  ONNX/VLM runtime backend when supported.
- `--ocr-threads N`
  CPU thread count for ONNX/VLM OCR.
- `--ocr-layout-model PATH`
  optional PP-DocLayoutV3 override.
- `--ocr-det-model PATH`, `--ocr-rec-model PATH`, `--ocr-dict PATH`
  not used by search ingest; raw PaddleOCR ONNX text recognition is intentionally
  out of scope because its quality was not reliable enough.
- `--ocr-threshold`, `--ocr-rec-threshold`, `--ocr-layout-conf`,
  `--ocr-max-size`
  carry existing OCR quality/performance controls.
- `--meta-prompt TEXT`
  prompt used by `llm-meta`.
- `--resize-width N`, `--no-resize`
  mirror the old `meta` image pre-processing controls. `llm-meta` defaults to
  resizing the longest edge to 768px.
- `--model`
  remains the vector embedding model hint for semantic indexing. It is not the
  image-recognition model.
- Image recognition provider/model for `llm-meta` resolve from the Chat image
  recognition preset, matching `image_understand`.

## CLI Smoke Fixtures

Use small real fixtures, not private photo folders:

```powershell
# OCR smoke fixture. Prefer existing OCR test images if stable.
npm run test:ocr:vl:layout:threads

# Search index smoke once adapter exists.
tanit-cli search index ./tests/fixtures/images --content image --backend vector --adapter ocr-local --ocr-mode layout --ocr-threads 8 --index-policy same-dir --fingerprint cheap --json

tanit-cli search search ./tests/fixtures/images --content image --mode fingerprint --query "table invoice total" --json

tanit-cli search detail --adapter ocr-local --path ./tests/fixtures/images/invoice.png --selector 'page:1' --include-detail page --json
```

Expected smoke assertions:

- index exits `0`;
- `search-manifest.json` records `content=image`;
- manifest records adapter ids and adapter options hash;
- `fingerprints.jsonl` contains `image-file` and OCR/meta records;
- OCR fingerprints include text snippets, confidence, layout labels, and bbox
  locators;
- `--mode fingerprint` returns image hits without loading OCR/VLM/embedding
  models;
- semantic search returns OCR/meta units after vector indexing;
- `search detail` can expand a page/block/bbox locator;
- failure cases persist skipped/error states so reruns do not silently spin.

## Implementation TODOs

Done:

- `image-sidecar` is registered as a source adapter.
- `src/search/source_adapter_image.cpp` emits `image-file` units with path,
  probe, size, sidecar facts, and image locators.
- `--content image --backend vector --adapter image-sidecar` indexes into
  `image.vstore`.
- `--mode fingerprint` can search image sidecar/path facts.
- `tests/orchestrator/test-search.mjs` covers image sidecar indexing and
  fingerprint retrieval.
- `ocr-local` emits page-level OCR units through PP-DocLayoutV3 layout detection
  plus the preferred OCR VLM; raw PaddleOCR ONNX recognition is not used.
- `llm-meta` emits page-level metadata units by calling `image_understand`
  directly with default or CLI prompt and optional auto-resize.
- Search CLI adapter options include `--meta-prompt`, `--resize-width`, and
  `--no-resize`; these are persisted in adapter options/cache settings when used
  with `llm-meta`.
- `search-manifest.json` now includes structured provenance for embedding
  backend/model/thread/GPU settings, adapter options, adapter option hashes,
  prompt hashes when a prompt is configured, and fingerprint sidecar path.
- OCR/meta units persist provider/model/backend-ish facts, prompt hashes, and
  relevant thread/GPU/resize settings in `docs.jsonl` and `fingerprints.jsonl`.

Next:

1. Emit stable image `SourceUnit` facts and locators for page/block/line
   units.
2. Teach `fingerprint.cpp` to summarize OCR text, labels, objects,
   dimensions, path slugs, and bbox/layout hints.
3. Implement `search detail` for `ocr-local` locators: page, block, line, bbox,
   and markdown table expansion.
4. Keep model-backed OCR/meta smoke coverage behind
   `PIXLWIZ_SEARCH_IMAGE_ADAPTERS=1` unless CI gets stable local VLM/router
   credentials.
6. Update `docs/search/search-next.md` supported content table and CLI examples
    once the first adapter is wired.

## Non-Goals For First Slice

- No visual embedding model yet.
- No object detection/indexing unless an existing backend is already stable.
- No image crop artifact generation in the first detail implementation.
- No automatic expensive LLM metadata generation during plain search; require
  explicit `search index` or explicit index creation policy.
- No replacement/edit actions for images yet.
