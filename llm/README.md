# llm/ — a tiny on-device LLM (an "Ollama-like" loader at small scale)

Loads a model **baked into the kernel image** and runs transformer inference
on the Pi to generate text.  This is not Ollama (no GGUF / llama.cpp / GPU /
big models) — it is the same idea (load a local model, run it on-device) at the
scale that bare-metal Xinu can handle.

## Model
`stories260K` from Karpathy's *llama2.c* tinyllamas — a Llama-2 architecture
model (dim 64, 5 layers, 8 heads, **GQA** 4 kv-heads, vocab 512, seq 512,
~260K params, fp32, ~1 MB).  Legacy llama2.c export: `Config` header +
weights including precomputed `freq_cis` (RoPE) + a shared classifier.

- `stories260K.bin`, `tok512.bin` — model + tokenizer (from
  https://huggingface.co/karpathy/tinyllamas/stories260K)
- `blob.S` — `.incbin`s both into `.rodata`
- `llm.c` — config/weight loader, the forward pass (RMSNorm, RoPE, GQA
  attention, SwiGLU, shared-classifier logits), a greedy sampler, a BPE
  tokenizer (encode + byte-fallback decode), and the `llm` shell command.

## Use
```
xinu-pi4$ llm                       # generate from BOS
xinu-pi4$ llm Lily went to the      # generate, conditioned on a prompt
```

## Notes / limits
- Built WITHOUT `-mgeneral-regs-only` (FP at EL1); `fsqrt` inline, own `exp`.
- The kernel runs with the **D-cache OFF**, so inference is slow — fine for
  this tiny model.  Generation drains the GENET RX ring each layer so a busy
  link can't overflow it while we compute (the wm loop is blocked meanwhile).
- Greedy decoding loops/repeats; temperature sampling would diversify it.
- Swapping in a bigger trained model is just a different blob (subject to the
  ~1 GB heap and the D-cache-off speed).
