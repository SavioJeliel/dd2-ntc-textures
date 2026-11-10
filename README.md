# DD2 NTC Textures

**English** · [Português](#português)

A Dragon's Dogma 2 mod that shrinks the game on disk by about **40 GB**: from roughly **90 GB to 50 GB**.

It removes most texture data from the game's PAK files. While you play, a REFramework plugin rebuilds those textures on the GPU from compressed [Neural Texture Compression (NTC)](https://github.com/NVIDIA-RTX/RTXNTC) data.

| | Size |
| --- | --- |
| Original game | 90.5 GB |
| Texture data removed from the PAKs | −43.6 GB |
| NTC data added | +3.9 GB |
| **Game with the mod** | **about 51 GB on disk** |

Textures lose some quality, since this is lossy compression. Loading works normally.

> ✅ **v1.1 supports game versions 4.1 and 4.2** (the current Steam version). The installer detects your version by itself.
>
> ✅ **AMD works:** the GPU self-test passed on an AMD Radeon RX 7900 XTX (80 of 80 textures rebuilt on the GPU).

## What happens when the game updates? (plain English)

Think of the game's big data file as a huge bookshelf, and each texture as a book in it.

- The mod keeps a **list of where each book is** and a **compressed copy of each book**. It removes the original books from the shelf to free space, and when the game wants a book, the mod hands it the copy.
- A game update usually **moves books to other spots on the shelf**, and sometimes **replaces a few books** with new ones. From 4.1 to 4.2, about 95% of the textures were exactly the same, they just moved.
- **The mod is careful:** before removing any book, it checks that the book is exactly the one it has a copy of. If a book is different or not where it expects, it just **leaves it alone**. So an update can make the mod save less space, but it won't break your game.
- **This version (v1.1)** knows the exact shelf positions of versions 4.1 and 4.2.
- **Next version (in progress):** the mod will learn the new positions **by itself**. You start the game once, the mod looks at where the books are now, and then the installer can free the space. Future game updates that only move textures around will then work without waiting for a new pack. Textures that the update actually changed still need to be compressed again, so those stay in the game until a new pack.

## Requirements

- **Game version 4.1 or 4.2.** The installer finds the plugin's hook points in `DD2.exe` and checks every texture block before it changes anything. Blocks that don't match are left untouched; if nothing matches, nothing is changed. Run `verify.bat` first to see what it would do on your copy.
- **[REFramework](https://github.com/praydog/REFramework) for DD2** (`dinput8.dll` in the game folder). Tested with v1.5.9.1.
- **The game on an NTFS drive.** The installer uses NTFS sparse files.
- **A DirectX 12 GPU.** Tested on an NVIDIA GeForce RTX 5060 (see [Tested hardware](#tested-hardware)). The installer self-tests your GPU first.
- **Windows PowerShell 5.1 or newer.** It ships with Windows 10 and 11.

## ⚠️ Read before installing

- **The change cannot be undone by the installer.** The texture blocks in the PAKs are zeroed. To undo, restore the original PAK files from your own backup. On Steam, *Verify integrity of game files* re-downloads them, about 44 GB. **Make a backup first if you want an easy way back.**
- **A game update breaks the mod.** Updates replace the PAKs. Wait for a pack made for the new build.
- **Don't copy the installed game folder to another drive or a ZIP.** A copy fills the removed blocks with zeros and takes 90 GB again. Install the mod on the destination instead.

## Why does Windows show "Size 90 GB" and "Size on disk 50 GB"?

The PAKs are now *sparse files*. Each PAK keeps its original declared size, which the game needs to find its data. The removed texture blocks are holes that NTFS does not store.

**"Size on disk" is the real space used.** The 40 GB is returned to the drive as free space, and other programs can use it. Playing the game only reads the PAKs, so they never fill back up.

## Install

1. Download **all** parts (`DD2-NTC-Textures-…-partNofM.zip`).
2. Extract every part into the **same folder**. You should get one `DD2-NTC-Textures` folder with `install.bat` and a `payload` folder inside.
3. Close the game.
4. Run **`verify.bat`** and pick the game folder, the one with `DD2.exe`. It only checks compatibility and changes nothing.
5. Run **`install.bat`** and pick the same folder. It takes a few minutes:
   1. It checks every file in the package.
   2. It runs the GPU self-test, then checks the game build and every texture block.
   3. It installs the plugin into `reframework\plugins`.
   4. It zeroes the blocks and verifies them.
6. Start the game as usual.

From PowerShell:

```
powershell -ExecutionPolicy Bypass -File install.ps1 -GamePath "D:\Games\Dragons Dogma 2" [-VerifyOnly]
```

## Something went wrong?

1. Run **`report.bat`** and pick the game folder. It creates `DD2-NTC-report-<date>.zip` on your Desktop with:
   - the installer logs, which every run saves in `logs\`, including the GPU self-test result;
   - the REFramework and plugin logs from the last two game sessions;
   - your GPU, driver, RAM and disk info;
   - the state of the PAK files;
   - the last DD2 crash events from Windows.
2. Open an issue on GitHub, or a bug report on Nexus. Attach the ZIP and say what happened and when: on install, at the menu, loading the save or in game.

The ZIP contains logs and system info only, no personal files. You can open it and check before sharing.

## Uninstall

1. Delete `reframework\plugins\dd2_ntc_bridge.dll` and `reframework\plugins\dd2_ntc_data`.
2. Restore `re_chunk_000.pak` and `dlc\re_dlc_stm_2593290.pak` from your backup, or verify the game files on Steam.

The plugin alone cannot run without the original PAKs, so step 2 is required.

## Known limits

- **Some textures stay in the PAKs.** About 2.2 GB of textures are left uncompressed. These are texture arrays and cubemaps, textures the game reads before the plugin loads, a few terrain textures, and formats without BC compression.
- **The plugin's texture cache has a dynamic cap:** 3% of RAM, between 256 MB and 1 GB.

## Support and next games

If this mod saved you space, you can buy me a coffee: **[buymeacoffee.com/saviojeliel](https://buymeacoffee.com/saviojeliel)** ☕

**Want this for another game?** Open a [game request](../../issues/new?template=game-request.yml) with the game and its install size. Every month I pick the next game from the requests:
- supporters' votes count first;
- the bigger the game, the more it gains, so very large games (60 GB and up) get priority;
- the engine must allow it: the textures have to live in package files the game reads by offset.

## Tested hardware

Developed and tested on an **NVIDIA GeForce RTX 5060**. The plugin rebuilds textures with DirectX 12 compute shaders from the NTC library, with Int8 weights and no vendor-specific tensor features.

| GPU | Result |
| --- | --- |
| NVIDIA GeForce RTX 5060 | Game 4.1: installed, menu, save and gameplay OK. Game 4.2: installed, title screen and new game OK |
| AMD Radeon RX 7900 XTX | GPU self-test passed (80/80 textures, ~3 ms each); in-game test pending |
| NVIDIA GeForce RTX 5070 Ti | `verify.bat` passed (GPU self-test and file check), reported by a user; in-game test pending |

Other DirectX 12 GPUs *should* work, since NVIDIA validates this NTC mode on GTX 10-series, AMD RX 6000 and Intel Arc and newer.

**The installer runs a GPU self-test before changing anything.** It rebuilds a sample of textures of every format on your GPU with the same code the plugin uses. If any of them fails, the installer stops and leaves the game untouched. Please report your GPU and driver version in an issue, whether it passes or fails.

## Building the plugin from source

The plugin source is in `src/bridge` (MIT). It needs these, all downloaded separately:
- the [RTXNTC SDK](https://github.com/NVIDIA-RTX/RTXNTC), under NVIDIA's license, which is not included here. `CMakeLists.txt` expects it in `../tools/RTXNTC`;
- MinHook and DirectXTex;
- Visual Studio 2022 and CMake.

The released ZIPs already contain the compiled plugin.

Build steps (Windows, Visual Studio 2022):
1. Put the RTXNTC SDK in `src/tools/RTXNTC`, MinHook and DirectXTex where `src/bridge/CMakeLists.txt` expects them.
2. `cmake -S src/bridge -B src/bridge/build -G "Visual Studio 17 2022" -A x64`, then `cmake --build src/bridge/build --config Release`. This builds `dd2_ntc_bridge.dll` and downloads `bc7enc.cpp` into `src/bridge/build`.
3. `cmake -S src/gpu-selftest -B src/gpu-selftest/build -G "Visual Studio 17 2022" -A x64`, then `cmake --build src/gpu-selftest/build --config Release`. This builds `ntc_gpu_bench.exe`, the GPU self-test the installer runs. It reuses the plugin's GPU code.

The release contains only three binaries: `dd2_ntc_bridge.dll` and `ntc_gpu_bench.exe`, both built from this source, and `libntc.dll`, NVIDIA's unmodified library from the RTXNTC SDK.

## Credits and licenses

- **The project's own code** (plugin source, installer, scripts) is under the [MIT License](LICENSE). The MIT license does **not** cover `libntc.dll` or the other third-party parts.
- **`libntc.dll`** (NVIDIA RTX Neural Texture Compression 0.10.1) belongs to NVIDIA Corporation and is protected by the [NVIDIA RTX SDKs License](NVIDIA-RTX-SDK-LICENSE.txt). It ships only in compiled form, together with this mod.
  - No NVIDIA source code is included.
  - The plugin only works with Dragon's Dogma 2 and does not expose the NTC library to other programs.
  - This mod is not sponsored or endorsed by NVIDIA.
- **Other components:** [REFramework](https://github.com/praydog/REFramework) by praydog (MIT, not included); [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause); [DirectXTex](https://github.com/microsoft/DirectXTex) (MIT); [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (MIT or public domain); [stb_dxt](https://github.com/nothings/stb) (public domain or MIT). The full list is in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt), which the installer also copies next to `libntc.dll` in the game folder.
- **The game:** Dragon's Dogma 2 © Capcom. This is an unofficial fan mod, not affiliated with Capcom. It ships no original game files, only compressed derivatives that need your own copy of the game.

---

## Português

Mod para Dragon's Dogma 2 que reduz o jogo em disco em cerca de **40 GB**: de uns **90 GB para 50 GB**.

Ele tira dos PAKs a maior parte dos dados de textura. Durante o jogo, um plugin do REFramework reconstrói essas texturas na GPU a partir de dados comprimidos com [Neural Texture Compression (NTC)](https://github.com/NVIDIA-RTX/RTXNTC).

As texturas perdem um pouco de qualidade, porque a compressão tem perda. O carregamento funciona normalmente.

> ✅ **A v1.1 funciona nas versões 4.1 e 4.2 do jogo**, que é a atual da Steam. O instalador descobre a sua versão sozinho.
>
> ✅ **Funciona em AMD:** o autoteste de GPU passou numa AMD Radeon RX 7900 XTX, com as 80 texturas reconstruídas pela GPU.

### E quando o jogo atualiza? (explicação simples)

Pense no arquivo grande do jogo como uma estante enorme, e em cada textura como um livro.

- O mod guarda uma **lista de onde fica cada livro** e uma **cópia compactada de cada um**. Ele tira os livros originais da estante para liberar espaço e, quando o jogo pede um livro, entrega a cópia.
- Uma atualização do jogo costuma **mudar os livros de lugar** na estante e, às vezes, **trocar alguns** por livros novos. Da 4.1 para a 4.2, cerca de 95% das texturas eram iguais: só mudaram de lugar.
- **O mod é cuidadoso:** antes de tirar um livro, ele confere se é exatamente aquele de que tem a cópia. Se o livro for diferente ou não estiver onde ele espera, **deixa onde está**. Uma atualização pode fazer o mod economizar menos espaço, mas não quebra o jogo.
- **Esta versão (v1.1)** conhece o lugar exato dos livros nas versões 4.1 e 4.2.
- **A próxima versão (em andamento)** vai **descobrir sozinha** os lugares novos. Você abre o jogo uma vez, o mod olha onde os livros estão agora, e o instalador libera o espaço. Atualizações que só mudam as texturas de lugar vão funcionar sem esperar um pacote novo. As texturas que a atualização realmente trocou ainda precisam ser compactadas de novo, então ficam no jogo até um pacote novo.

### Requisitos

- **Versão 4.1 ou 4.2 do jogo.** O instalador acha no `DD2.exe` os pontos de que o plugin precisa e confere cada trecho de textura antes de mexer em qualquer coisa. Trechos que não batem ficam intactos; se nada bater, nada é alterado. Rode o `verify.bat` antes para ver o que ele faria na sua cópia.
- **[REFramework](https://github.com/praydog/REFramework) para DD2** (`dinput8.dll` na pasta do jogo). Testado com a v1.5.9.1.
- **Jogo num disco NTFS.**
- **GPU com DirectX 12.** Testado numa NVIDIA GeForce RTX 5060.

### ⚠️ Antes de instalar

- **O instalador não desfaz a mudança.** Os trechos de textura são zerados nos PAKs. Para voltar ao original, restaure os PAKs de um backup seu. Na Steam, a opção *Verificar integridade dos arquivos* baixa os PAKs de novo, cerca de 44 GB. **Faça um backup antes, se quiser um jeito fácil de voltar.**
- **Atualização do jogo quebra o mod.** A atualização troca os PAKs. Espere um pacote feito para a build nova.
- **Não copie a pasta do jogo instalado para outro disco ou para um ZIP.** A cópia preenche os trechos removidos com zeros e volta a ocupar 90 GB. Em vez disso, instale o mod no destino.

### Por que o Windows mostra "Tamanho 90 GB" e "Tamanho em disco 50 GB"?

Os PAKs agora são *arquivos esparsos*. Cada PAK mantém o tamanho original declarado, de que o jogo precisa para achar os dados. Os trechos de textura removidos viram buracos que o NTFS não grava.

**O "tamanho em disco" é o espaço que o jogo ocupa de fato.** Os 40 GB voltam a ficar livres no disco, e outros programas podem usá-los. Jogar só lê os PAKs, então eles nunca voltam a encher.

### Instalação

1. Baixe **todas** as partes (`DD2-NTC-Textures-…-partNofM.zip`).
2. Extraia todas na **mesma pasta**. Deve aparecer uma pasta `DD2-NTC-Textures`, com `install.bat` e uma pasta `payload` dentro.
3. Feche o jogo.
4. Rode o **`verify.bat`** e escolha a pasta do jogo, a que tem o `DD2.exe`. Ele só confere a compatibilidade e não altera nada.
5. Rode o **`install.bat`** e escolha a mesma pasta. Leva alguns minutos.
6. Abra o jogo como sempre.

### Deu problema?

1. Rode o **`report.bat`** e escolha a pasta do jogo. Ele cria `DD2-NTC-report-<data>.zip` na Área de Trabalho, com:
   - os logs do instalador, inclusive o resultado do autoteste de GPU;
   - os logs do REFramework e do plugin das duas últimas sessões;
   - a GPU, o driver, a RAM e o disco;
   - o estado dos PAKs;
   - as últimas quedas do jogo registradas no Windows.
2. Abra uma issue no GitHub, ou um bug report no Nexus. Anexe o ZIP e conte o que aconteceu e quando: na instalação, no menu, ao carregar o save ou durante o jogo.

O ZIP só tem logs e informações do sistema, nenhum arquivo pessoal. Dá para abrir e conferir antes de enviar.

### Desinstalação

1. Apague `reframework\plugins\dd2_ntc_bridge.dll` e `reframework\plugins\dd2_ntc_data`.
2. Restaure `re_chunk_000.pak` e `dlc\re_dlc_stm_2593290.pak` do seu backup, ou verifique os arquivos na Steam.

O plugin não funciona sem os PAKs originais, então o passo 2 é obrigatório.

### Apoie e peça o próximo jogo

Se o mod te economizou espaço, você pode me pagar um café: **[buymeacoffee.com/saviojeliel](https://buymeacoffee.com/saviojeliel)** ☕

**Quer isso em outro jogo?** Abra um [pedido de jogo](../../issues/new?template=game-request.yml) com o nome do jogo e o tamanho da instalação. Todo mês escolho o próximo entre os pedidos:
- os votos de quem apoia contam primeiro;
- quanto maior o jogo, mais ele ganha, então jogos acima de 60 GB têm prioridade;
- o motor precisa permitir: as texturas têm que ficar em arquivos de pacote que o jogo lê por offset.

### Hardware testado

O mod foi desenvolvido e testado numa **NVIDIA GeForce RTX 5060**. O plugin usa compute shaders do DirectX 12 da biblioteca NTC, com pesos Int8 e sem recursos exclusivos de fabricante. O `verify.bat`, com autoteste de GPU e conferência dos arquivos, também passou numa AMD Radeon RX 7900 XTX e numa NVIDIA GeForce RTX 5070 Ti, segundo os testers. Nessas duas, falta o teste dentro do jogo. Outras GPUs com DX12 *devem* funcionar.

**Antes de alterar qualquer coisa, o instalador faz um autoteste na sua GPU.** Ele reconstrói uma amostra de texturas de todos os formatos com o mesmo código do plugin. Se alguma falhar, ele para e não mexe no jogo. Conte numa issue qual é a sua GPU e a versão do driver, passando ou não.

### Licenças

- **O código do projeto** está sob a licença MIT.
- **O `libntc.dll`** pertence à NVIDIA Corporation e segue a NVIDIA RTX SDKs License. Ele vai só compilado, junto com o mod, sem nenhum código-fonte da NVIDIA.
- **O plugin** só funciona com o Dragon's Dogma 2.
- **Créditos:** a lista completa está em `THIRD_PARTY_NOTICES.txt`. O instalador também copia esse arquivo para a pasta do jogo.
