# kvlog

Banco chave-valor em C++17, gravado num log append-only. É a mesma ideia do Bitcask (o motor
do Riak), feita do zero pra eu entender de verdade o que um banco precisa fazer pra não perder dado
quando o processo morre no meio de uma escrita.

```
kvlog dados put jogador:76561190000000001 '{"kills":12,"mortes":3}'
kvlog dados get jogador:76561190000000001
kvlog dados del jogador:76561190000000001
kvlog dados compact
```

Sem dependência nenhuma além da biblioteca padrão. Compila no Linux (g++/CMake) e no Windows (MinGW ou MSVC).

## Como funciona

```
 put("a", "1")  put("b", "2")  put("a", "3")  del("b")
      │              │              │             │
      ▼              ▼              ▼             ▼
 ┌──────────┬──────────────┬──────────────┬──────────────┐
 │ a = 1    │ b = 2        │ a = 3        │ b = ✝ lápide │   000001.log  (só cresce)
 └──────────┴──────────────┴──────────────┴──────────────┘
                                   ▲
 índice em memória:  a ─▶ (arquivo 1, posição 30, 25 bytes)
```

- **Escrever** é acrescentar um registro no fim do arquivo ativo. Nada é sobrescrito no lugar.
- **Ler** é buscar a chave no índice (um `unordered_map`) e fazer uma leitura no disco.
- **Apagar** é gravar uma lápide.
- Quando o arquivo ativo passa de 4 MiB, ele é fechado (com fsync) e um novo começa.
- **Compactar** reescreve os arquivos fechados só com o valor mais recente de cada chave.

Cada registro tem um CRC-32 e o tamanho da chave e do valor:

```
crc32 (4) │ tamanho da chave (4) │ tamanho do valor (4) │ chave │ valor
```

## O que acontece quando o processo morre

Essa é a parte que me interessou. Existem três lugares onde a queda pode acontecer.

**No meio de uma escrita.** O fim do arquivo fica com meio registro. Ao abrir, o banco relê todos os
arquivos e confere o CRC de cada registro. No arquivo ativo, o primeiro registro inválido marca até
onde a escrita chegou: dali pra frente tudo é descartado e o arquivo é truncado. Num arquivo *fechado*
isso não pode acontecer (ele recebeu fsync antes de fechar), então registro inválido ali é
corrupção de verdade, e o banco recusa abrir (`CorruptionError`) em vez de devolver dado errado.

**No meio de uma compactação.** A compactação dos arquivos 1..N funciona assim:

1. copia os registros vivos pra `N.tmp` e faz fsync
2. renomeia `N.tmp` → `N.compact`. **Esse rename é o commit**
3. apaga `1.log` .. `N.log`
4. renomeia `N.compact` → `N.log`

Se cair antes do passo 2, sobra um `.tmp` que o `open` apaga. Os dados continuam nos arquivos
originais. Se cair depois do 2, o `open` encontra o `.compact` e refaz os passos 3 e 4. Como apagar
o que já foi apagado e renomear de novo dá no mesmo, a queda pode acontecer em qualquer ponto.

Uma armadilha que o teste cobre: a compactação não copia as lápides. Se ela apagasse só *alguns*
arquivos antigos e o `open` relesse o resto, uma chave apagada podia voltar do mundo dos mortos
(o valor antigo num arquivo que sobrou, a lápide num que foi apagado). Por isso o `open` nunca relê
um conjunto pela metade: ele termina a troca primeiro.

**Durabilidade.** Com `Sync::Always` (padrão), cada `put` só retorna depois do fsync: o dado sobrevive
a queda de energia. Com `Sync::Never`, ele vai só até o sistema operacional (fflush): sobrevive a
`kill -9`, mas não a queda de energia.

## Testes

`tests/tests.cpp` (60 verificações, sem framework):

- escrita cortada em **cada byte** do último registro: o `open` descarta só aquele registro, e o log
  continua aceitando escrita depois
- bit trocado no fim do arquivo ativo (descarta) e num arquivo fechado (recusa abrir)
- leitura de um valor corrompido com o banco aberto (o CRC é conferido em toda leitura)
- queda logo depois do commit da compactação, inclusive com parte dos arquivos antigos já apagada,
  conferindo que a chave apagada não volta
- `.tmp` de compactação abandonada, rotação de arquivo, compactar duas vezes seguidas

`tests/kill9.sh` faz o teste com processo de verdade: grava chaves com fsync, imprimindo cada
confirmação, leva um `kill -9` num momento aleatório e depois confere se toda chave confirmada
sobreviveu e se o que está no disco é um prefixo sem buraco. Numa das rodadas o processo morreu
com 3 bytes de um registro já escritos:

```
rodada 3: morto depois de 14623 confirmações -> ok: 14623 confirmadas, 14623 no disco, 0 bytes descartados na recuperação
rodada 4: morto depois de 63945 confirmações -> recuperação: descartados 3 bytes de uma escrita incompleta no fim do log
ok: 63945 confirmadas, 63945 no disco, 3 bytes descartados na recuperação
rodada 5: morto depois de 45225 confirmações -> ok: 45225 confirmadas, 45226 no disco, 0 bytes descartados na recuperação
```

(Na rodada 5 tem uma chave a mais no disco do que confirmada: ela foi gravada, mas o processo
morreu antes de avisar. Isso é permitido. O contrário, confirmar e perder, não seria.)

O primeiro bug que os testes pegaram foi meu: o ponteiro pro cabeçalho do registro era pego antes
de concatenar chave e valor na `std::string`. Quando ela realocava, o CRC ia pra memória já liberada
e o registro saía com CRC zerado. Registros curtos passavam (cabiam no buffer interno da string),
e os maiores eram descartados na reabertura.

## Números

`kvlog <pasta> bench 200000`: chaves de 16 bytes, valores de 100 bytes.

| | Linux (WSL2, ext4) | Windows 10 (NTFS) |
|---|---:|---:|
| escrita, só fflush | 127 mil/s | 31 mil/s |
| leitura aleatória | 212 mil/s | 24 mil/s |
| compactação (48,8 → 24,4 MiB) | 0,53 s | 3,9 s |
| abrir e reconstruir o índice | 0,43 s | 1,2 s |
| escrita com fsync a cada put | 39/s | 504/s |

O fsync manda em tudo: com ele, cada escrita espera o disco. É por isso que bancos de verdade fazem
*group commit* (juntam várias escritas num fsync só). O número do WSL é baixo porque o disco do
Linux ali é um arquivo virtual em cima do NTFS, e cada fsync atravessa as duas camadas.

## Limites (de propósito)

- O índice inteiro fica em memória: são as chaves mais uns 16 bytes cada. Pra milhões de chaves
  pequenas, dá; pra bilhões, não.
- Uma thread por instância. Não tem lock.
- Não tem busca por faixa (`scan de a até b`): o índice é hash. Pra isso seria uma LSM-tree.
- Ao abrir, o banco relê todos os arquivos. O Bitcask grava *hint files* pra pular essa parte;
  aqui 200 mil chaves levam meio segundo, então ficou de fora.

## Compilando

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
bash tests/kill9.sh build/kvlog 10
```

No Windows sem CMake tem o `build.bat`, que usa o g++ do MinGW/TDM-GCC.

## Estrutura

```
src/
  store.hpp / store.cpp   o banco: formato do registro, índice, rotação, compactação e recuperação
  file.hpp / file.cpp     E/S com offset de 64 bits, fsync de arquivo e de pasta (Windows e Linux)
  crc32.hpp               CRC-32
  main.cpp                linha de comando, bench, fill/verify do teste de kill -9
tests/
  tests.cpp               testes de unidade e de queda
  kill9.sh                teste com kill -9 de verdade
```

## Licença

MIT
