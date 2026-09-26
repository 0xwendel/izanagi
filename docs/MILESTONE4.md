# milestone 4 — host integration layer

## fluxo e ownership

`Present` seleciona a swapchain, garante RTV e constrói um `FrameContext` na stack.
os ponteiros COM e a HWND são borrowed: o hook gráfico continua dono das referências
que já adquiriu. o contexto não faz `AddRef`/`Release` e deixa de existir antes da
callback retornar. serviços não devem guardar o contexto nem seus ponteiros.

`runtime_services::Tick(frame)` é o ponto de entrada dos serviços por frame. ele
produz somente telemetria escalar. a GUI lê uma cópia de `SnapshotData` e usa o RTV
emprestado para desenhar. o hook não enumera módulos nem interpreta PE.

## módulos

`modules::Initialize` e o primeiro `Refresh` rodam na WorkerThread, antes de ativar
os hooks. a WorkerThread executa `Refresh` a cada 5 segundos. `Tick` e `Find` usam
o cache; nunca enumeram módulos. `Refresh` monta uma nova lista fora do lock e troca
a lista sob mutex. falha de enumeração esvazia o cache e registra `last_error`.

somente basename é aceito em `Find`. a comparação usa `CompareStringOrdinal` com
`bIgnoreCase=TRUE`; não modifica paths. resultados são cópias temporais: o
`HMODULE` não possui referência persistente e pode ficar obsoleto após retorno.
para dereference futuro, o consumidor deverá obter sua própria referência e validar
novamente o módulo. unload é observado no refresh seguinte.

o enumerador usa `EnumProcessModulesEx`, fixa temporariamente cada candidato com
`GetModuleHandleExW(FROM_ADDRESS)`, obtém `MODULEINFO` e nome, valida os headers e
libera a referência antes de publicar metadata. o parser limita offsets pelo
`SizeOfImage` conhecido e usa `VirtualQuery` antes de copiar cada header. valida DOS,
NT64, AMD64, optional header, tamanho de imagem e espaço da section table. não lê
sections nem data directories.

## threads e shutdown

- WorkerThread: inicialização, refresh periódico, início do shutdown e teardown.
- thread de `Present`: `FrameContext`, `Tick`, GUI; pode variar entre frames.
- thread da WndProc: input ImGui; sem acesso ao registry.

o estado do serviço é atômico. o snapshot de telemetria e o cache de módulos usam
locks separados. nenhum lock desses é mantido durante `Present` original,
`ResizeBuffers` original ou WndProc original.

na saída, a WorkerThread marca `shutting_down` primeiro. o hook é desabilitado e
suas callbacks são drenadas pelo mecanismo existente. somente após sucesso desse
drain a GUI e os serviços são destruídos. a ordem de serviços é inversa à
inicialização: telemetria e depois ModuleRegistry. se o drain falhar, a DLL fica
carregada e os serviços não são destruídos sob callbacks possivelmente ativas.

falha de enumeração é degradada: o overlay permanece operacional, com contador zero
e código de erro. `runtime_services::Initialize` não falha por ausência temporária
de módulos. nenhuma integração específica do host foi incluída.

## build

MSVC x64, C++20, `/MT` (`/MTd` em Debug). usa somente Windows SDK e as dependências
já existentes. nova biblioteca de link: `psapi.lib` (APIs de enumeração de módulos).
