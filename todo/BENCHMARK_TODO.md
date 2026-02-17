# Benchmarking Analysis - CRITICAL ISSUES

**Status**: METODOLOGIA FLAWED - Resultados Invalidam Conclusoes
**Created**: 2026-02-17
**Author**: Code Review

---

## EXECUTIVE SUMMARY

Os benchmarks atualmente publicados NAO sao validos para conclusao de tese porque a metodologia e fundamentalmente falha. O codigo mede overhead de subprocesso, nao latencia de transporte DDS vs HTTP.

**Resultado atual**: DDS 2.55x mais rapido e INCORRETO - os dados reais mostram o oposto porque a metodologia esta medindo o loop de polling, nao a inferencia.

---

## CRITICAL - Metodologia Invalida Resultados

### 1. Subprocess Por Request - OverheadMassivo

Arquivo: benchmark_thesis.py:66-93

Para CADA request DDS:
1. fork() + exec() - criar processo
2. Carregar binario test_client na memoria
3. Inicializar CycloneDDS (participant, topics, reader, writer)
4. Esperar discovery DDS (pode levar segundos)
5. Enviar request
6. Esperar response (polling 1s)
7. Sair e limpar DDS

Impacto: Cada request DDS adiciona 3-10 segundos de overhead que NAO existe no HTTP\!

Fix: Criar cliente DDS persistente (uma vez, reuse para todos os requests)

---

### 2. Variavel de Speedup Invertida

Arquivo: benchmark_thesis.py:216-223

Problema:
speedup = dds_mean / http_mean

Se speedup > 1, DDS e MAIS LENTO, mas o codigo printa DDS faster\!

Fix: speedup = http_mean / dds_mean (se >1, DDS e mais rapido)

---

### 3. Polling Loop de 1 Segundo - Piso Artificial

Arquivo: dds/benchmark_final.cpp:73-88

Problema: sleep(1) cria piso de 1000ms

Dados mostram:
- DDS latency ~2020ms = EXATAMENTE 2 x 1000ms
- std = 1-3ms (impossivel para inferencia LLM\!)

O benchmark esta medindo o polling loop floor, nao a latencia real\!

Fix: Reduzir polling para 1ms

---

### 4. Tokens Estimados Incorretamente

Arquivo: benchmark_thesis.py:84-85

prompt_tokens hardcoded como 0

O servidor NAO esta retornando contagem de tokens na resposta DDS.

---

### 5. Missing QoS - Perda de Dados Sob Carga

Arquivo: dds/benchmark_final.cpp:113-121

Falta: dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);

QoS mismatch pode causar perda de samples sob carga alta.

---

## CRITICAL - Memory Leaks

### 6. Vazamento de Memoria em benchmark_final.cpp

Cada request vaza ~200+ bytes:
- req.request_id = dds_string_dup() - NUNCA LIBERADO
- req.model = dds_string_dup() - NUNCA LIBERADO
- req.messages._buffer - NUNCA LIBERADO

Fix: Adicionar cleanup apos dds_write()

---

### 7. Memory Leaks em test_client.cpp

Mesmo problema: dds_string_dup() allocations nunca freed.

dds_delete(participant) nao libera strings C\!

---

## HIGH - Problemas de Codigo

### 8. UUID Nao RFC 4122 Compliant

Arquivo: dds/benchmark_final.cpp:23-36

Nao define version (4) bits
Nao define variant (RFC 4122) bits

---

### 9. Paths Hardcoded

Arquivo: benchmark_thesis.py:70

/mnt/e/TI/git/llama.cpp_dds/dds/test_client - nao portavel

---

### 10. Warmup Inconsistente

Para DDS com subprocess, warmup executa 2x full process spawn+init
HTTP warmup reutiliza conexao - nao e fair comparison

---

## Summary de Correcoes

| Priority | Issue | Impact |
|----------|-------|--------|
| CRITICAL | Subprocess per request | Metodologia invalida |
| CRITICAL | Speedup calculation | Conclusoes erradas |
| CRITICAL | Polling 1s floor | Medindo polling |
| CRITICAL | Missing QoS HISTORY | Perda de dados |
| HIGH | Memory leaks | OOM |
| HIGH | UUID non-compliant | Incompatibilidade |
| MEDIUM | Hardcoded paths | Nao portavel |

---

## Re-run Recomendado

Apos correcoes, executar benchmark com:
- Cliente DDS persistente (sem subprocess)
- Polling 1ms
- 50+ requests por prompt type
- Contagem real de tokens

---

## Referencias

- CycloneDDS QoS: https://docs.cyclonedds.io/latest/core/qos/
- RFC 4122 UUID: https://tools.ietf.org/html/rfc4122

