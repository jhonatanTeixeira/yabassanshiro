# Histórico de trabalho na branch `main`

Registro completo de tudo que foi commitado por **Jhonatan Teixeira** (`jhonatan.teixeira@gmail.com`)
na branch `main` deste fork, na ordem em que aconteceu. Cobre os 4 commits autorais que existem
nessa branch, o que cada um mudou tecnicamente, por quê, e o que ficou pendente.

> **Escopo**: só `main`. Este fork tem outras branches (`perf/r36s-improvements`,
> `feature/portal-trace-clean`, `perf/dynarec-linux`) com trabalho bem maior — dynarec x86_64,
> threads dedicadas para SH-2 Slave/SCU DSP, `portal_trace` para recompilação estática — que
> **não está em `main`**. Ver [Fora de escopo](#fora-de-escopo-outras-branches) no fim deste
> documento.

## Contexto: por que essas mudanças existem

`main` seguia o upstream do libretro (`f448097b`, "Merge pull request #325 from cscd98/webos64")
até `750a561d`. A partir daí o trabalho passou a ser guiado por um objetivo único, documentado em
[`CLAUDE.md`](../CLAUDE.md): **reduzir o uso de CPU do interpretador de ~32% para ~10% de um core**,
medido com `ps -o %cpu` no processo `retroarch` rodando *Magic Knight Rayearth (USA)* num build
`platform=unix` (x86_64, interpretador puro, sem dynarec, sem threads de worker — nada disso existe
em `main`).

A metodologia adotada foi: catalogar exaustivamente onde o tempo de CPU realmente vai (três
documentos de análise, granularidade por granularidade) antes de otimizar às cegas, e então
implementar os achados diretamente no código. Os três catálogos de análise foram commitados junto
com a primeira leva de otimizações:

- [`docs/every_pixel.md`](every_pixel.md) — inventário de tudo que roda **por pixel/texel** nos
  dois renderers (`vidsoft.c` software e `vidogl.c` OpenGL).
- [`docs/per_deciline.md`](per_deciline.md) — tudo que roda **por deciline** (1/10 de scanline,
  ~2600-3130×/frame) no loop principal de `yabause.c`.
- [`docs/once_a_frame.md`](once_a_frame.md) — trabalho que deveria rodar **uma vez por frame** mas
  na prática se repete por pixel/texel/instrução/acesso-à-memória (o achado com o resumo mais
  direto de "hoisting targets").

Esses três docs foram escritos como *o achado*, e os commits seguintes são *a implementação do
achado*. O restante deste documento segue essa ordem cronológica.

## Linha do tempo

| # | Commit | Data | Resumo |
|---|--------|------|--------|
| 1 | [`750a561d`](#1-750a561d--limpeza-do-loop-de-emulação-e-re-pacing-por-subsistema) | 2026-08-13 08:09 | Remove trabalho morto/redundante do loop de emulação, re-paceia cada subsistema pro seu próprio clock, quebra um rendezvous lock-step entre thread de emulação e de áudio |
| 2 | [`4bf503c9`](#2-4bf503c9--cache-de-textura-entre-frames--fix-de-bug-de-overflow) | 2026-08-13 09:55 | Implementa os achados de `once_a_frame.md`: cache de textura entre frames, dirty-tracking, upload de RBG sob demanda; corrige um bug de overflow de array incidental |
| 3 | [`378c1ebd`](#3-378c1ebd--compute-shader-rbg-por-padrão-dirty-tracking-granular-e-cycle-accounting-direto) | 2026-08-13 13:27 | Liga por padrão o compute shader GLES 3.1 de RBG (com fallback seguro), refina o dirty-tracking do VDP1 para páginas de 4KB, elimina o parâmetro de saída `cycle` na maioria das chamadas de memória |
| 4 | [`cd84c65c`](#4-cd84c65c--read-ahead-assíncrono-de-chd-e-instrumentação-de-diagnóstico) | 2026-08-15 10:52 | Read-ahead assíncrono de hunks de CHD numa thread dedicada, instrumentação temporária de diagnóstico no pipeline de áudio, limpeza de warnings de build |

Total agregado: **32 arquivos distintos tocados, ~4865 linhas adicionadas, ~207 removidas** (contando
apenas os 4 commits acima; docs e scripts inclusos).

---

## 1. `750a561d` — Limpeza do loop de emulação e re-pacing por subsistema

**Mensagem original**: *"Removed dead and redundant work from the emulation loop (profiler,
no-op syncs, per-frame texture/CD/memory-decode rebuilds), re-paced each emulated subsystem to
its own natural clock instead of stepping everything per-deciline, and broke a lock-step thread
rendezvous that was serializing the emulation and audio threads"*

Este é o commit fundacional: 19 arquivos, +1856/-111 linhas — inclui os três docs de análise,
o novo `CLAUDE.md` do projeto, e a primeira leva de implementação.

### O achado mais caro de todos: o "spin" do SCSP não dormia

[`scsp.c`](../yabause/src/scsp.c) `ScspAsynMainCpuTime()` — a thread consumidora do áudio
assíncrono — media, por profiling, **~30% do custo total do emulador**, sem fazer nenhum trabalho
real. Dois bugs empilhados:

1. O loop de espera nunca de fato dormia: girava até 200.000 iterações antes de cair num sleep de
   verdade, assumindo que esse intervalo seria maior que qualquer espera realista. Não era — a
   thread principal incrementa o contador a cada deciline (~6.3μs), então o spin sempre saía pela
   checagem do contador primeiro e o sleep virava código morto. Resultado: spin quente permanente
   num core inteiro.
2. Acordava a qualquer mudança do contador, mas uma amostra de áudio precisa de `samplecnt` (256)
   ciclos de M68K e um deciline só entrega ~71 — ou seja, ~3 de cada 4 wake-ups caíam direto no
   loop de trabalho sem ter o suficiente acumulado, e voltavam a girar sem ter feito nada.

Corrigido esperando pela condição que de fato importa (uma amostra inteira de ciclos acumulada) e
cedendo o core de verdade (`YabThreadUSleep(50)`) enquanto espera, em vez de queimá-lo girando —
ver [`scsp.c:5527`](../yabause/src/scsp.c#L5527).

### `ASYNC_SCSP`: documentado o trade-off, mantido desligado por enquanto

[`scsp.h`](../yabause/src/scsp.h) ganhou um comentário extenso explicando a escolha entre rodar
SCSP+M68K numa thread separada (bom em dispositivo multi-core fraco tipo R36S, onde a thread
principal já é o teto de framerate) vs. síncrono na thread principal (melhor para "CPU total" em
desktop, mas empurra tudo pro mesmo core que já está no limite). Nesse commit o modo async
continuava **desligado** (`//#define ASYNC_SCSP`) — só ligado no commit seguinte.

### Batching: cada subsistema no seu próprio clock, não mais tudo por deciline

[`yabause.c`](../yabause/src/yabause.c) `YabauseEmulate()`:

- **SH-2 Slave**: em vez de `SH2Exec(SSH2, ...)` a cada deciline (~2600-3130×/frame), os ciclos são
  acumulados em `sh2_slave_cycle_batch` e disparados **uma única vez por frame**, em VBlankIN,
  estritamente antes do render VDP1/VDP2 em VBlankOUT. A contabilidade interna de FRT/WDT dentro de
  `SH2Exec()` é exata independente de como o total é fatiado entre chamadas — o que muda é só a
  granularidade de interleaving com o Master. Risco aceito e documentado: um jogo que faça handshake
  próprio via RAM compartilhada *dentro* de um frame entre Master e Slave (fora do FRT
  cross-trigger, que continua funcionando) pode ver estado stale do Slave por um frame.
- **SMPC + CD-block** (`SmpcExec`/`Cs2Exec`): batching de deciline (~2600-3130×/frame) para
  **uma vez por scanline** (~10x menos chamadas) — ainda bem mais fino que os próprios thresholds
  internos desses subsistemas (SMPC ~83μs, CD-block 333ms/6.6-16.6ms). Em [`cs2.c`](../yabause/src/cs2.c)
  as duas checagens de threshold (`_statuscycles`/`_periodiccycles`) viraram `while` em vez de `if`,
  para lidar defensivamente com um delta batched que cruze mais de um período numa única chamada —
  marcado no doc como o mais arriscado dos dois batchings (CD-block tem histórico de regressão
  nesse código).
- `M68KSync()` deixou de ser chamado quando `ASYNC_SCSP` está ativo (nesse commit ainda não estava,
  mas o guard já foi adicionado) — é um no-op nesse modo, então a chamada (~2600-3130×/frame) era
  puro overhead de call/return.

### Rendezvous lock-step quebrado entre thread de emulação e áudio

`YabauseInit()`: as filas `q_scsp_frame_start`/`q_scsp_finish` tinham profundidade 1, formando um
rendezvous completo por frame — a thread principal não passava de VBlankIN até o SCSP terminar o
frame inteiro de áudio, e o SCSP não começava o próximo frame até ser liberado pela principal.
Nenhuma das duas conseguia trabalhar no frame N+1 enquanto a outra ainda estava no frame N: as
threads *alternavam* em vez de *sobrepor*, e o tempo de frame virava `(main + áudio)` em vez de
`max(main, áudio)` — medido em dispositivo como duas threads travadas em ~75% cada, com dois cores
ociosos, a 40fps. Corrigido para profundidade 2 (uma folga: áudio pode ficar até um frame atrasado,
que é exatamente o que o handshake precisa pra não divergir arbitrariamente) e primeira posição
"pré-preenchida" pra estabelecer o offset de um frame desde o início.

### Outras limpezas neste commit

- **Profiler desligado de vez**: `PROFILE_START`/`PROFILE_STOP` (`profile.c`) rodavam de verdade em
  build de release — `strcmp`-based tag lookup + `clock()` várias vezes por deciline × 60fps.
  `-DDONT_PROFILE` adicionado no [Makefile](../yabause/src/libretro/Makefile).
- **Idle-loop detection reativado**: `sh2idle.c` existia mas estava desabilitado incondicionalmente
  por um `#ifdef` que também guardava, sem relação nenhuma, a feature de execução a partir do
  cache-as-RAM (`0xC0000000+`). Trocado por um checagem em runtime (`PC` fora dessa região) em
  [`sh2int.c`](../yabause/src/sh2int.c#L3153) — jogos de Saturn gastam bastante tempo em polling de
  VBlank/HBlank/DMA/SCU, exatamente o padrão que esse detector existe para acelerar.
- **`SH2sleep` fast-forward**: a instrução `SLEEP` só soma 3 ciclos e re-busca a si mesma pra
  sempre até uma interrupção — interrupções só são atendidas no limite de chamada de
  `SH2InterpreterExec()`, não no meio do loop. Em vez de re-executar a cada 3 ciclos, consome o
  resto do budget de ciclos da chamada de uma vez.
- **Cache LRU multi-slot para hunks de CHD**: o cache de 1 slot (`current_hunk_id`) thrashava toda
  vez que o padrão de leitura alternava entre duas posições do disco (ex.: CD-DA intercalado com
  leitura de dados do jogo em outro lugar) — cada troca forçava uma re-descompressão LZMA completa
  do hunk (~19.5KB). Ampliado para 64 slots em [`cd-libretro.c`](../yabause/src/cd-libretro.c).
- **Tabela precomputada de custo de ciclo de memória**: `GET_MEM_CYCLE_R`/`_W` recalculavam a
  classificação de região via cadeia de `switch` a cada acesso à memória do SH-2 — substituído por
  lookup numa tabela (`ReadCycleList`/`WriteCycleList`, `0x10000` entradas) preenchida uma vez em
  `MappedMemoryInit()`. VDP2 RAM continua dinâmico (`CYCLE_DYNAMIC_VDP2`) porque seu custo depende
  de estado em runtime.
- **`m68k_counter` virou `std::atomic` com `release`/`acquire`** em vez do `seq_cst` padrão —
  padrão single-producer/single-consumer não precisa de ordenação total; mais barato tanto em x86
  (elide a fence exigida por `seq_cst`) quanto em ARM ([`Counter.cpp`](../yabause/src/Counter.cpp)).
- **`cell_scroll_data` deixa de ser lido do VRAM por scanline** quando o `VIDCore` ativo não é o
  software renderer — essa tabela só é consumida por `vidsoft.c`, que nunca roda no build
  libretro/OGL. 88 leituras de VRAM por scanline (~23.000/frame) eliminadas
  ([`vdp2.cpp`](../yabause/src/vdp2.cpp)).
- **`Vdp2GetAlpha` sai cedo quando `specialcolormode==0`** (caso comum) em vez de ler `CCCTL` e
  cair no `switch` sem casos correspondentes.
- **Memoização de plano/página em `Vdp2DrawMapTest`**, replicando um padrão que já existia no
  sibling `Vdp2DrawMapPerLine` — a maioria dos tiles consecutivos fica dentro do mesmo
  mapa/plano/página (32×32 ou 64×64 células), então recalcular o endereçamento a cada tile é
  desperdício.
- **`fixVdp2Regs->SPCTL` deixa de ser relido/remascarado por texel** em `Vdp1ReadTexture` — hoisted
  para fora do loop já que `fixVdp2Regs` é um snapshot congelado por passe de desenho.
- **`NO_LINK_GL=1`** adicionado ao Makefile — permite compilar sem linkar GL/GLES/EGL, resolvendo
  contra as bibliotecas do host em tempo de carregamento; necessário pra cross-compilar (ex.
  aarch64 pra R36S a partir de x86).
- **Versão do build passa a incluir timestamp** (`GIT_VERSION`), não só o hash do HEAD commitado —
  antes, builds com mudanças não commitadas no working tree ficavam com a mesma string de versão
  de builds com código genuinamente diferente.
- **A primeira leva do cache de textura entre frames** também entra aqui, em `vidogl.c`
  (`VIDOGLVdp2DrawStart`): o atlas compartilhado VDP1/VDP2 era resetado e reconstruído do zero todo
  frame, mesmo para conteúdo 100% estático. Detecção de "sujeira" via `memcmp` de snapshot dos
  registradores VDP1/VDP2 (saneados para excluir campos de *status* de hardware que mudam todo
  frame por design — `EDSR`, `addr`/`COPR`, `TVSTAT` — que fariam o memcmp sempre "dar diferente" e
  derrotar o cache por completo) mais as flags `A0-B1_Updated`/`Vdp2ColorRamUpdated`/
  `g_Vdp1RamUpdated` (nova, setada em toda escrita de CPU/DMA em Vdp1Ram).

---

## 2. `4bf503c9` — Cache de textura entre frames + fix de bug de overflow

**Mensagem original**: *"direct implementations of findings from docs/once_a_frame.md — the
texture caching, dirty-tracking, and RBG upload optimizations. The HashTable fix is an incidental
bug discovered during the struct changes."*

7 arquivos, +858/-9. Continuação direta do cache de textura introduzido no commit anterior,
fechando as duas lacunas que ele deixava.

### Bug encontrado e corrigido: MSB-shadow silenciosamente desligava em cache hit

`_Ygl->msb_shadow_count_[]` era incrementado dentro de `Vdp1ReadTexture()` — mas essa função **não
roda** quando o texture cache dá hit. Resultado: o passe de sombra MSB (gateado nessa contagem, em
`ygles.c`) silenciosamente parava de rodar em frames com cache hit, causando sombras de sprite
piscando (aparecendo e sumindo). Corrigido movendo a contagem para logo após
`Vdp1ReadCommand()` nas três funções de desenho de sprite
(`VIDOGLVdp1NormalSpriteDraw`/`ScaledSpriteDraw`/`DistortedSpriteDraw` em
[`vidogl.c`](../yabause/src/vidogl.c)) — por **comando**, não por decode, já que o comando roda
sempre, com ou sem cache hit.

### Cor de paleta deixa de invalidar o atlas inteiro (exceto quando precisa)

Mudanças em Color RAM geralmente **não** deveriam invalidar o atlas: os decoders gravam índices de
paleta nos texels, não cores resolvidas — a resolução final acontece no fragment shader contra
`cram_tex`, que já é mantida incrementalmente. Um fade de paleta zerava o atlas inteiro todo frame
à toa. Exceção: modo de color-calc especial 3 (`SFCCMD`), onde `Vdp2GetAlpha()` lê a cor crua da
Color RAM e a grava no byte de alpha do texel — só nesse caso uma mudança de CRAM ainda invalida.

### Upload de textura por linhas sujas, não o atlas inteiro

Antes, `YglTmPush()` fazia `glTexSubImage2D` de `[0, yMax)` todo frame — com o atlas agora
sobrevivendo entre frames, `yMax` virou uma marca d'água permanente, então cada push reenviava o
atlas inteiro de volta pra GPU mesmo sem nada ter mudado (a economia de decode virava custo de
upload). Adicionado `dirty_y0`/`dirty_y1` em [`ygl.h`](../yabause/src/ygl.h), atualizados em
`YglTMAllocate()`, e o upload em `YglTmPush()` (`ygles.c`) passa a cobrir só o intervalo sujo.

### RBG: pula upload de 512KB de VRAM quando nada mudou

`RBGGenerator::update()` (`ygl_texture.cpp`) copiava os 512KB inteiros de `Vdp2Ram` pra uma SSBO
todo frame incondicionalmente. Nova flag `g_Vdp2RamDirtyForRbg`, tirada de um snapshot das flags
`A0/A1/B0/B1_Updated` em `Vdp2DrawRBG0` (antes delas serem zeradas) — o `memcpy` só roda quando
essas flags indicam escrita real.

### Bug incidental: overflow de 1 elemento em `HashTable`

`YglgetHash()` mascara com `& HASHSIZE` (`0xFFFF`), o que pode legitimamente retornar `0xFFFF` —
índice **um a mais** que o fim de um array `[HASHSIZE]`, corrompendo o que vinha depois na struct
(`CashLink[0].addr`). Escrita fora dos limites pré-existente, achada durante as mudanças de struct
deste commit. Corrigido para `[HASHSIZE + 1]` em `ygl.h`.

---

## 3. `378c1ebd` — Compute shader RBG por padrão, dirty-tracking granular e cycle accounting direto

**Mensagem original** (formatada como lista P0-P3 no commit):
- **P0 (implementado)**: compute shader GLES 3.1 ligado por padrão para camadas de rotação RBG,
  com fallback seguro em erro de compilação.
- **P1 (implementado)**: dirty-tracking granular de páginas de 4KB para VDP1.
- **P2 (implementado)**: acumulação direta em `CurrentSH2->cycles` nos acessores de barramento de
  memória.
- **P3 (pendente)**: fragment shader de fallback GLES 3.0 para RBG.

16 arquivos, +1962/-48.

### P0 — Compute shader RBG default-on, com fallback de verdade

`g_rbg_use_compute_shader` passa de `0` para `1` em [`libretro.c`](../yabause/src/libretro/libretro.c).
Mas a mudança que torna isso seguro está em [`ygl_texture.cpp`](../yabause/src/ygl_texture.cpp): antes,
um erro de compilação ou link do shader chamava `abort()` (depois de escrever o shader-source num
arquivo `tmp.cpp` para debug) — ou seja, qualquer driver GLES 3.1 com um bug de compilador
derrubava o emulador inteiro. Agora: loga o erro, libera o shader/programa, seta
`_Ygl->rbg_use_compute_shader = 0` e retorna `0` — o chamador cai de volta pro caminho sem compute
shader em vez de crashar. Isso é o que torna seguro ligar por padrão: dispositivos com suporte
capenga de compute shader degradam graciosamente.

### P1 — Dirty-tracking de VDP1 por página de 4KB

Antes, qualquer escrita em Vdp1Ram setava uma única flag global (`g_Vdp1RamUpdated`), então
qualquer escrita — incluindo só a tabela de comandos, que fica numa região separada da área de
textura/padrão — invalidava o atlas inteiro. [`vdp1.cpp`](../yabause/src/vdp1.cpp) ganha
`MarkVdp1RamDirty(addr)`: mantém um bitmap de páginas sujas de 4KB (`g_Vdp1RamDirtyPages[2]`,
128 páginas cobrindo os 512KB de Vdp1Ram) e uma segunda flag mais específica,
`g_Vdp1TextureRamUpdated`, setada só quando o endereço escrito está de fato na região de
textura/padrão (`addr >= 0x04000`). `vidogl.c` troca `g_Vdp1RamUpdated` por
`g_Vdp1TextureRamUpdated` na condição de `content_dirty` — jogos que atualizam a lista de comandos
todo frame (comum) mas não tocam textura deixam de invalidar o atlas.

### P2 — Cycle accounting direto, sem parâmetro de saída

`GET_MEM_CYCLE_R`/`_W` ganham um segundo modo: se `cycle` (o ponteiro de saída) é `NULL`, em vez de
escrever nele, acumula direto em `CurrentSH2->cycles` (ver [`memory.c`](../yabause/src/memory.c) e
[`memory.h`](../yabause/src/memory.h)). Elimina o "escreve no ponteiro de saída, chamador lê o
ponteiro e soma em cycles" em ~40 call sites de `sh2int.c` — vira uma soma direta. Assinatura em
C++ usa parâmetro default (`u32 * cycle = NULL`), mantendo compatibilidade com o lado C via
`#ifdef __cplusplus`.

### Outros itens deste commit

- Reversão do fix do dirty-tracking de Color RAM do commit anterior ficou **redundante** com a
  chegada do dirty-tracking de página de VDP1 — o comentário explicativo foi removido do código
  (a lógica em si, `cram_affects_texels`, permanece).
- Dois arquivos com BOM UTF-8 (`memory.c`, `ygl_texture.cpp`) tiveram o BOM removido.
- `m68kmake` (binário gerado do Musashi) removido do commit anterior é excluído de novo aqui —
  artefato de build que não devia ter sido commitado.

---

## 4. `cd84c65c` — Read-ahead assíncrono de CHD e instrumentação de diagnóstico

**Mensagem original**: *"several small performance improvements"*

4 arquivos, +189/-39. O mais recente (2026-08-15), e o único que mistura otimização real com
instrumentação temporária de debug ainda presente no HEAD.

### Read-ahead assíncrono de hunks de CHD numa thread dedicada

`ISOCDReadAheadFAD()` em [`cd-libretro.c`](../yabause/src/cd-libretro.c) era um no-op puro. Agora
sobe uma thread worker (`ChdWorkerThreadFunc`, `pthread_cond_wait`/`signal`) que, ao ser chamada
com o FAD atual, pré-descomprime até 2 hunks à frente (offsets +8 e +16 setores) direto no cache
LRU compartilhado — protegido por `g_chd_cache_mutex`, separado do mutex de sinalização
(`g_chd_readahead_mutex`) pra não segurar o worker enquanto ele descomprime. O cache LRU em si
dobrou de tamanho: `CHD_HUNK_CACHE_SLOTS` de 64 para 128 slots.

### Instrumentação de diagnóstico (ainda presente, marcada como temporária)

Em [`scsp.c`](../yabause/src/scsp.c) e [`yabause.c`](../yabause/src/yabause.c): contadores
`g_dbg_active_slot_sum`/`g_dbg_dsp_step_sum`/`g_dbg_cdda_active_sum` (quantos dos 32 slots de voz
de hardware estão realmente tocando som vs. sendo iterados à toa, quantos passos de programa DSP
rodam) e um split de tempo de parede da thread SCSP entre `MM68KExec`/`new_scsp_exec` (compute
genuíno) vs. espera (`YabThreadUSleep`) — impresso via `fprintf(stderr, "[SCSP-DIAG] ...")` uma vez
por frame. Em `yabause.c`, `SyncCPUtoSCSP()` loga quando a thread principal fica bloqueada mais de
2ms esperando o áudio do frame anterior. Objetivo declarado nos comentários: distinguir "a thread
de áudio está genuinamente limitada por throughput de síntese" de "a thread de áudio está sendo
esfomeada pelo pacing da thread principal" — **ainda não removida**, marcada
`TEMPORARY DIAGNOSTIC ... Remove once root-caused` nos dois arquivos.

### Limpeza de build e correções de warning

- Vários `malloc()`/`sscanf()` em `cd-libretro.c` ganham casts explícitos (`(u8 *)`, `(ChdInfo *)`,
  `(char*)` etc.) — provavelmente pra silenciar warnings de `void*`→tipo em modo C++ ou com um
  compilador mais estrito.
- Comentários de código morto (trechos comentados antigos de cálculo de FAD/pregap/postgap)
  removidos de `LoadCHD()`/`ISOCDReadSectorFADFromCHD()`.
- [Makefile](../yabause/src/libretro/Makefile): comentário extenso documentando uma tentativa
  **revertida** de `-flto` no target `arm64_cortex_a53_gles3` — mediu ~28% de corte no custo de
  síntese de efeitos DSP do SCSP (LTO permite inline entre `scsp.c`/`scspdsp.c`, que `-O3` sozinho
  não cruza), mas foi revertido: o codebase tem estado cross-thread compartilhado via globais
  simples não-atômicas/não-voláteis (thread de SCSP/M68K, worker de read-ahead de CD), e a
  visibilidade whole-program do LTO permitiu tratar legalmente algumas dessas leituras como
  loop-invariant, parando de observar escritas de outra thread — reproduzido como uma flag de
  "avançar diálogo" que nunca mais aparecia depois da primeira vez. Não reabilitar sem antes
  auditar/converter cada flag cross-thread pra `std::atomic` ou `volatile`.

---

## Documentos e ferramentas adicionados (não-código)

Junto com as mudanças de código, esta sequência de commits trouxe uma quantidade grande de
material de análise e planejamento — parte dele já consumido pelas implementações acima, parte
ainda como plano para trabalho futuro (em `main` ou nas branches irmãs):

| Arquivo | O que é |
|---|---|
| [`docs/every_pixel.md`](every_pixel.md) | Inventário de tudo que roda por pixel/texel nos dois renderers |
| [`docs/per_deciline.md`](per_deciline.md) | Inventário de tudo que roda por deciline no loop principal |
| [`docs/once_a_frame.md`](once_a_frame.md) | Achados de trabalho redundante que deveria ser 1x/frame |
| [`docs/framebuffer_overhaul_deepseek.md`](framebuffer_overhaul_deepseek.md) | Análise independente (modelo DeepSeek) da arquitetura de framebuffer, com dados empíricos de benchmark no R36S |
| [`docs/framebuffer_overhaul_gemini.md`](framebuffer_overhaul_gemini.md) | Segunda análise independente (modelo Gemini), mesmo objetivo |
| [`docs/framebuffer_overhaul_qwen.md`](framebuffer_overhaul_qwen.md) | Terceira análise independente (modelo Qwen), revisão focada em CPU |
| [`docs/r36s_next_moves.md`](r36s_next_moves.md) | Perfil do dispositivo R36S (RK3326/Mali-G31) e próximos passos |
| [`docs/r36s_concrete_implementation_plan.md`](r36s_concrete_implementation_plan.md) | Plano concreto de implementação lendo o código-fonte real, cruzando com os achados acima |
| [`docs/r36s_definitive_refactoring_guide.md`](r36s_definitive_refactoring_guide.md) | Síntese final de todos os docs de R36S num guia único |
| [`scripts/deploy_r36s.sh`](../scripts/deploy_r36s.sh) | Script de cross-compile + deploy automatizado para o dispositivo R36S via SSH |
| [`CLAUDE.md`](../CLAUDE.md) | Guia do projeto para o Claude Code (objetivo, build, arquitetura) |
| [`QWEN.md`](../QWEN.md) | Guia equivalente para o assistente Qwen |

Vale notar: os docs de R36S (`framebuffer_overhaul_*`, `r36s_*`) usam uma metodologia e um alvo
diferentes do baseline declarado em `CLAUDE.md` — medem `core_avg`/`video_avg` em milissegundos via
`retrorun3 --benchmark` num RK3326/Mali-G31 real, não `ps -o %cpu` no build x86_64/unix. Os dois
esforços convergem no mesmo conjunto de causas-raiz (cache de textura ausente, VDP1 dirty-tracking
grosseiro, compute shader RBG desligado), o que é o motivo dessas otimizações terem sido
implementadas em `main` mesmo tendo sido descobertas num contexto de R36S.

---

## Estado atual e pendências

- **P3 do plano de `378c1ebd` continua pendente**: fragment shader de fallback GLES 3.0 para RBG
  (dispositivos sem GLES 3.1/compute shader ainda caem no caminho de CPU antigo, não num fragment
  shader dedicado).
- **Instrumentação de diagnóstico do SCSP ainda presente** em `scsp.c`/`yabause.c` (commit
  `cd84c65c`) — código explicitamente marcado `TEMPORARY DIAGNOSTIC`, imprime em `stderr` a cada
  frame, ainda não removido nem teve sua causa raiz documentada como resolvida.
- **`-flto` permanece desabilitado** no target `arm64_cortex_a53_gles3` até que os globais
  cross-thread sejam auditados e convertidos para `std::atomic`/`volatile`.
- **Nenhuma medição pós-mudança do baseline x86_64/unix está registrada no repositório** — os
  documentos descrevem o baseline (~32%) e a meta (~10%), mas não há um valor de `ps -o %cpu`
  atualizado commitado depois dessas otimizações para confirmar quanto do gap foi fechado nesse
  build específico. As medições que existem (`core_avg`/`video_avg`) são do R36S, não do x86_64.
- **`ASYNC_SCSP` está ligado por padrão** desde `4bf503c9` — correto para o alvo multi-core fraco
  (R36S), mas significa que medir "CPU total" num desktop vai mostrar dois cores ocupados em vez de
  um, o que é o trade-off documentado em `scsp.h`, não uma regressão.

---

## Fora de escopo: outras branches

Estas branches existem no remoto mas **não fazem parte de `main`** — citadas aqui só para
contexto, caso a intenção seguinte seja documentá-las também:

- **`perf/r36s-improvements`** (ponta em `9ce24c03`, "Add README documenting this fork's
  performance work and profiling tools") — inclui, entre outras coisas, um dynarec x86_64,
  threads dedicadas para SH-2 Slave e SCU DSP, e a eliminação da thread de áudio do SCSP em favor
  de um sync single-thread guiado por ciclo.
- **`feature/portal-trace-clean`** (ponta em `a9ae349a`, "Add portal_trace: structured real-time
  JSONL capture for static recompilation") — ferramenta de captura estruturada para um projeto de
  recompilação estática.
- **`perf/dynarec-linux`** (ponta em `59c9f3fd`, "dynarec for linux x86_64").

Nenhuma dessas está mesclada em `main` no momento em que este documento foi escrito.
