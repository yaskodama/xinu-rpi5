// llm/blob.S — bake the tiny LLM (Karpathy stories260K, llama2.c legacy
// format) and its tokenizer into the kernel image as .rodata blobs.  Paths
// are relative to the build CWD (compile/).
.section .rodata
.balign 8
.global model_data
model_data:
.incbin "../llm/stories260K.bin"
.global model_data_end
model_data_end:

.balign 8
.global tok_data
tok_data:
.incbin "../llm/tok512.bin"
.global tok_data_end
tok_data_end:
