# milestone 7 — spatial model, camera e projeção

## estado

a infraestrutura espacial read-only compila em release x64 e os testes de
schema, entities e matemática espacial passam. a posição mundial usa a rota
refletida `entity.m_pGameSceneNode -> CGameSceneNode.m_vecAbsOrigin`.
essa rota foi validada in-process no cs2: o inspector mostrou
`C_LightEnvironmentEntity`, `spatial: success` e world `(224, -320, -192)`.
a câmera permanece
`unavailable`: nenhum objeto ou método estrutural que forneça a matriz de
view-projection foi validado. por isso o cs2 ainda não publica projeções válidas.
o contrato degrada sem afetar os serviços m1–m6.

## evidência e fronteira ABI

inspeção estática read-only da build local encontrou as strings
`m_pGameSceneNode`, `CGameSceneNode` e `m_vecAbsOrigin` em `client.dll`.
o inspector in-process confirmou `m_pGameSceneNode: CGameSceneNode*` em `+0x330`
para `CCSPlayerController`.
origem: `game/csgo/bin/win64/client.dll`, SHA-256
`9b4f46dbd6a433163b39d7ea0123c321b1ad6d95ceedd40ae121312464833549`.
timestamp PE `0x6ab6d1db`, `SizeOfImage=0x2998000`; `engine2.dll` mantém
timestamp `0x6ab58b8e`, `SizeOfImage=0x971000`. são os perfis aceitos no m6.
as strings indicam disponibilidade de metadata, mas não provam que a posição
retornada seja correta no processo vivo. `MatrixWorldToScreen` e
`Source2EngineToClient001` também aparecem no binário; o nome isolado não
estabelece calling convention, índice de vtable, lifetime nem layout de matriz.
nenhuma chamada foi inferida a partir dessas strings.

`TransformResolver` concentra os nomes concretos em `source2/abi/spatial.cpp`.
`m_vecAbsOrigin` é `VectorWS`, não `Vector`: a primeira validação in-process
retornou `type mismatch` em todas as entidades. o
[dump do schema client do cs2](https://s2v.app/SchemaExplorer/cs2/client/CGameSceneNode)
indica `VectorWS` em `+0xC8`; a tabela local da `client.dll` confirma esse offset
e o próximo field em `+0xD4`, diferença de 12 bytes. o binding foi corrigido;
o resultado in-process confirmou uma posição finita.
ele resolve offsets pelo registry público do m5 e mantém bindings por classe e
generation. na troca de generation, descarta todos os bindings. o pointer da
entidade é obtido por `EntityView::read_bound`, que reutiliza a validação de
handle do m6. a leitura de 12 bytes do scene node usa `ReadBoundMemory` do m6,
com tamanho de classe, overflow, `VirtualQuery` e cópia protegida. o pointer do
node é relido depois da posição. isso reduz, mas não elimina, a race com a
engine; não há ownership nem trava da simulação. valor nulo, field ausente,
tipo diferente e valor não finito produzem status distintos.

## convenção matemática

`Matrix4x4::m[row][column]` guarda linhas em ordem crescente (row-major).
vetores são colunas; `clip = view_projection * {world.x, world.y, world.z, 1}`.
quando view e projection vierem separadas, a composição normalizada deverá ser
`projection * view`. a camada pura usa NDC com x e y em `[-1, 1]`, z em `[0, 1]`,
origem de pixel no canto superior esquerdo e y de tela crescente para baixo.
`screen.x = (ndc.x + 1) * width/2` e
`screen.y = (1 - ndc.y) * height/2`. `w <= 1e-5` retorna `behind camera` sem
divisão. não há transpose implícito; um futuro adapter de câmera terá de provar
a ordem e handedness da matriz da engine antes de marcar `valid=true`.

`ValidCamera` exige dimensões entre 1 e 16384, elementos finitos, matriz não
zero e posto completo pelo escalonamento de linhas. uma câmera inválida retorna
`invalid camera`. pontos offscreen preservam coordenadas projetadas sem clamp.
`Vec3` mantém eixos e unidades da engine; nenhum eixo é invertido no transform.

## execução e snapshots

no `Present`, `runtime_services::Tick` registra o frame m6 e copia apenas a
dimensão do backbuffer para `CameraService`. o hook DXGI obtém largura/altura
quando cria o RTV; durante `ResizeBuffers` zera os valores e publica os novos
no próximo RTV. não há `GetBuffer`, alocação ou varredura espacial por frame.

na WorkerThread, `PollServices` executa schema, entities e spatial nessa ordem.
o m7 consome `EntityFrameSnapshot`, revalida cada handle com m6 e constrói um
novo `SpatialFrameSnapshot` com valores próprios. lê a câmera uma vez por
construção. publica `shared_ptr<const ...>` apenas após preencher o snapshot.
falha global publica snapshot vazio com frame de origem identificável; nunca
reclassifica dados antigos como atuais. a GUI mostra contagens, duração,
posição da entidade selecionada e status da projeção, sem desenhar no mundo.

o scan m6 ocorre na WorkerThread com intervalo mínimo de um segundo. o m7
processa cada nova geração do snapshot m6 ou mudança da viewport, em O(n) e sem lookup de
field por entidade após binding. uma captura no cs2 mostrou 360 entidades
processadas, 356 transforms válidos e `spatial tick: 334,901 ms`; o scan m6
levou 174,666 ms e o `render tick` exibiu 0,0000 ms nesse frame.
shutdown destrói spatial e camera antes de entities; depois entities antes de
schema. nenhum snapshot contém ponteiros da engine.

## validação pendente no cs2

1. observar responsividade do worker em mapa cheio e após refresh de schema;
2. identificar por interface documentada ou RE verificável a fonte da matriz,
   sua convenção e seu lifetime; só então habilitar `CameraSnapshot.valid`;
3. validar projeção textual em resize, alt-tab e mudança de resolução.

os testes sintéticos cobrem centro, esquerda, direita, acima, abaixo, fora da
viewport, atrás da câmera, `w` singular, identidade, matriz zero/não finita,
viewport inválida e ponto NaN. eles não provam a ABI nem a aquisição da câmera.
