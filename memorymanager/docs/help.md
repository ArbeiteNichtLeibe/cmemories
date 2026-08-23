# Alocação no MemoryManager: estratégia de thread_id aleatório

## Problema
`Thread already has an active loan` ao usar `thread_id` fixo (ex: 0) no `mm->allocate()`.

## Solução
Gerar um `uint64_t thread_id` pseudo‑aleatório (ex: `(rand()<<32) | rand()`) e tentar alocar. Se falhar com erro “active loan”, tenta outro ID. Repetir até 10 vezes.

## Exemplo
```cpp
for (int attempt = 0; attempt < 10; ++attempt) {
    uint64_t tid = (rand()<<32) | rand();
    if (tid == 0) tid = 1;
    if (mm->allocate(tid, 1, ...)) {
        // sucesso
        break;
    }
    // se erro != "active loan", abortar
}
