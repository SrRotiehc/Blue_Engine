# BlueEngine — referência de funções

```python
import BlueEngine          # o .so da sua versão do Python precisa estar na mesma pasta (ou no sys.path)
```

**Convenções:** cores de 0 a 255 · ângulos em graus · cada modelo é transformado nesta ordem:
escala → rotação (X, depois Y, depois Z) → posição.
Funções marcadas com `*` são escritas no `main.py` embutido; as outras vêm do C++.

---

## Janela e loop

| Função | O que faz |
|---|---|
| `start(width=1600, height=800, title="Blue Engine")` `*` | Cria a janela e o contexto OpenGL. Dá erro se for chamada duas vezes. |
| `quit()` `*` | Fecha a janela e libera os modelos. Pode ser chamada mais de uma vez. |
| `running()` → `bool` `*` | `True` até a janela ser fechada ou `stop()`/`quit()` serem chamados. |
| `stop()` `*` | Pede para encerrar (`running()` passa a ser `False`). |
| `windowClosed()` → `bool` `*` | `True` só quando o jogador fechou a janela (X, Alt+F4…). |
| `windowTitle(title)` | Muda o título da janela. |
| `pollEvents()` `*` | Lê os eventos do frame. Chame **uma vez por frame**, no começo do loop. |
| `deltaTime()` → `float` `*` | Segundos entre os dois últimos `pollEvents()` (limitado a 0.1 s). |

## Entrada

Os nomes das teclas são os do SDL: `"w"`, `"space"`, `"escape"`, `"left shift"`, `"up"`… (nome inválido → `ValueError`).

| Função | O que faz |
|---|---|
| `keyDown(key)` → `bool` `*` | `True` enquanto a tecla está segurada. |
| `keyPressed(key)` → `bool` `*` | `True` só no frame em que a tecla foi apertada (ignora o auto-repeat). |
| `mousePos()` → `(dx, dy)` `*` | Quanto o mouse se moveu neste frame. |
| `setMouseCap(on)` | `True` esconde o mouse e passa a ler só o movimento (modo FPS). |
| `mouseCap()` → `bool` | Diz se o mouse está capturado. |

## Desenho

Ordem de um frame: `clear(...)` → `modelo.render()` de cada modelo → `flip()`.

| Função | O que faz |
|---|---|
| `clear(color=(0,0,0))` | Limpa a tela com a cor e posiciona a câmera. Vem primeiro no desenho. |
| `flip()` | Mostra na janela o que foi desenhado. |
| `setShade(color)` | Cor global multiplicada em tudo (inclusive planos sem textura). `(255,255,255)` = sem alteração. |
| `setWireframe(on)` | `True` desenha só as linhas das faces. |

## Câmera — objeto `BlueEngine.camera`

Chamada **com** valores define; **sem** valores devolve o atual.

| Função | O que faz |
|---|---|
| `camera.pos()` / `pos(x, y, z)` / `pos((x, y, z))` | Posição. Começa em `(0, 0, 5)`. |
| `camera.yaw()` / `yaw(graus)` | Rotação horizontal. Positivo = vira para a direita. |
| `camera.pitch()` / `pitch(graus)` | Rotação vertical, limitada a ±89°. Positivo = olha para baixo. |
| `camera.fov()` / `fov(graus)` | Campo de visão (padrão 60). |
| `camera.rotate(yaw=0, pitch=0)` | **Soma** aos ângulos atuais (bom para o mouse). |

Para andar na direção em que a câmera olha (no plano horizontal):
`frente = (sin(yaw), 0, -cos(yaw))` e `direita = (cos(yaw), 0, sin(yaw))`, com `yaw` em radianos.

## Modelos

```python
loadModel(obj="", texture="", pos=(0,0,0), scale=1.0, rotation=(0,0,0), transparent=False) -> Model
```

| `obj` | `texture` | Resultado |
|---|---|---|
| ✔ | ✔ | Modelo `.obj` com a textura. |
| ✔ | — | Modelo `.obj` sem textura (cor do `setShade`). |
| — | ✔ | **Plano 2D** (“papel”): 1 de altura, largura na proporção da imagem, no plano XY virado para +Z, visível dos dois lados. |
| — | — | **Plano em branco** 1×1, liso, na cor do `setShade`. Com `transparent=True` fica invisível. |

- `transparent=True` faz o canal alfa da textura valer (PNG com áreas transparentes). Sem textura, o único efeito é tornar invisível o plano em branco.
- `scale` aceita um número ou `(x, y, z)`. Positivo multiplica, negativo divide (`-2` = metade), `0` mantém o tamanho original.
- Erros (`RuntimeError`): `start()` não foi chamado, ou o `.obj`/a textura não pôde ser carregado.
- Só `loadModel()` sem nada **não** dá erro: cria o plano em branco.

### Métodos do `Model`

Assim como na câmera: com valores define, sem valores devolve.

| Método | O que faz |
|---|---|
| `pos()` / `pos(x, y, z)` / `pos((x, y, z))` | Posição. |
| `scale()` / `scale(x, y, z)` / `scale(valor_ou_(x,y,z))` | Escala (o valor lido já é o fator final: `scale(-2)` lê `0.5`). |
| `rotation()` / `rotation(x, y, z)` / `rotation((x, y, z))` | Rotação em graus. |
| `move(x=0, y=0, z=0)` | **Soma** à posição. |
| `rotate(x=0, y=0, z=0)` | **Soma** à rotação. |
| `transparent()` / `transparent(on)` | Lê / liga / desliga a transparência. |
| `render()` | Desenha o modelo (entre `clear()` e `flip()`). |
| `unload()` | Libera a textura da GPU (acontece sozinho ao fechar a engine). |

**Dica de transparência:** desenhe primeiro os modelos opacos e depois os transparentes, do mais longe para o mais perto. Pixels totalmente transparentes não dependem da ordem; as bordas semitransparentes dependem.

## Formatos aceitos

- **Modelo `.obj`:** linhas `v`, `vt` e `f` (formatos `v`, `v/vt`, `v/vt/vn` e `v//vn`, índices negativos também). Faces com 4 ou mais cantos são divididas em triângulos. Normais e materiais (`.mtl`) são ignorados.
- **Textura:** qualquer imagem que o Pillow abra (PNG, JPG…), convertida para RGBA.

## Funções internas (não use direto)

`_start`, `_quit`, `_pollRaw`, `_keyHeld`, `_scancode`, `_ticks` — são a camada crua de SDL que o `main.py` usa para montar as funções acima.

---

## Compilar

Requisitos (Debian/Ubuntu): `sudo apt install g++ libsdl2-dev libgl-dev` e, para **cada** Python:
`python3.X -m pip install pybind11 numpy pillow` (numpy e Pillow também são necessários na hora de rodar).

```bash
bash build.sh                       # python3.11, 3.12, 3.13 e 3.14
bash build.sh python3.12            # só uma versão
bash build.sh python3.12 python3.14 # várias
```

O `BlueEngine.cpp` e o `main.py` precisam estar na mesma pasta (o `main.py` é embutido dentro do `.so` na compilação; se mudar, compile de novo). Cada `.so` só funciona na versão do Python para a qual foi compilado.

Comando manual para uma versão:

```bash
g++ -O2 -std=c++17 -shared -fPIC -fvisibility=hidden \
    $(python3.12 -m pybind11 --includes) $(pkg-config --cflags sdl2) \
    BlueEngine.cpp -o BlueEngine$(python3.12 -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))") \
    $(pkg-config --libs sdl2) -lGL
```
