# NX Audio MixPilot

Protótipo VST3 com duas funções selecionáveis dentro do mesmo plugin. Use **Track** nas pistas e **HUB** no master ou em um bus. As instâncias compartilham medições e comandos quando estão na mesma sessão numérica e no mesmo processo do host.

## Compilar no Visual Studio 2022

Instale a carga de trabalho **Desenvolvimento para desktop com C++**, o SDK do Windows, Git e CMake 3.22 ou superior. Em um terminal do Visual Studio, entre na pasta deste projeto e execute:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

A configuração gera a solução em `out/windows-x64/NXMixPilot.sln` e usa o commit `29396c22c93392d6738e021b83196283d6e4d850` do JUCE 8.0.12. Para configurar com uma cópia local dessa versão, use `cmake --preset windows-x64 -DJUCE_ROOT=C:/caminho/JUCE`. Abra a solução, selecione **Release | x64** e compile **NXMixPilot_VST3**.

O bundle é `out/windows-x64/NXMixPilot_artefacts/Release/VST3/NX Audio MixPilot.vst3`. Copie a pasta completa para `C:\Program Files\Common Files\VST3` e faça a busca de plug-ins no Studio One. Este projeto inclui o código e a configuração de build para Windows. O build MSVC e o comportamento no Studio One 8 ainda precisam ser verificados nesse host.

Para rodar só os testes de DSP no Linux, com CMake, Ninja e um compilador C++17:

```bash
cmake --preset dsp-tests
cmake --build --preset dsp-tests
ctest --preset dsp-tests
```

## ANALYZER automático

Em **Track**, o editor calibra a pista aberta e oferece controle rotativo de trim. No **HUB**, os medidores do mix bus, o diagnóstico tonal e a tabela mostram as medições da sessão. Use a roda do mouse sobre a tabela para percorrer mais pistas.

No HUB, selecione a sessão e o modo **Calibração técnica** ou **Balanço musical**. Reproduza um trecho representativo e clique em **ANALYZER**. O botão fica verde durante a análise. Após cerca de 1,2 segundo, ele aplica um trim individual a cada Track ativo da sessão com medição atual e margem suficiente. O seletor de escopo continua valendo para **Capturar prévia**. Use **Cancelar** durante a análise ou **Desfazer** depois da aplicação. O ponto verde na coluna Trim identifica o ajuste aplicado pelo MixPilot durante a sessão.

Marque a caixa **Foco** no fim da linha da pista de referência no HUB para selecioná-la e ativar o **Balanço musical**. Depois, com as pistas em reprodução, clique em **ANALYZER**.

No balanço musical, a pista de **Foco** continua sendo a âncora. A análise tonal permanece visível no espectro para avaliação auditiva. Ela sinaliza sobreposições candidatas e deixa o áudio sem EQ automático.

## Calibração técnica

1. Insira o plugin antes dos processadores que devem receber o nível calibrado. No Track, escolha a **Sessão** e, se aplicável, atribua um **Grupo**. Para evitar misturar projetos abertos no mesmo processo, use um número de sessão distinto para cada projeto.
2. No HUB, selecione a mesma sessão, escolha **Calibração técnica** e ajuste **0 VU (RMS)**, **Headroom** e **Limite de trim**. O padrão **0 VU = −18 dBFS RMS** é um nível nominal de trabalho. A referência pode ser alterada de −36 a −6 dBFS RMS. A leitura usa RMS digital pré-trim, com suavização de 350 ms e medição de energia dos dois canais. O plugin usa o nível nominal como referência de escala. A resposta balística de um medidor VU analógico fica fora deste protótipo.
3. Reproduza um trecho representativo durante pelo menos um segundo. Escolha **Uma pista**, **Um grupo** ou **Todas as pistas** e clique em **Capturar prévia**. A prévia fica fixa. Em grupo, as pistas ativas recebem o mesmo deslocamento em dB, preservando o balanço relativo entre elas. A âncora é o maior RMS pós-trim do grupo, e o maior pico de amostra restringe o ajuste. Pistas sem medição válida ficam de fora.
4. Use **Ouvir B** para escutar o ajuste e **Voltar a A** para retornar aos trims anteriores. A audição B usa a diferença de nível do próprio ajuste. Clique em **Aplicar trim** durante a reprodução para gravar os valores, ou em **Cancelar prévia** para descartá-los. **Desfazer** restaura os valores anteriores enquanto a instância HUB existir. O estado do projeto guarda os trims aplicados. O histórico de desfazer dura até a remoção da instância HUB ou o fechamento do projeto.
5. O trim é um ganho linear com rampa de 35 ms. Ele preserva a forma dos transientes e a relação entre picos e RMS depois da rampa. A margem usa **pico de amostra** recente, em dBFS. RMS, LUFS e true peak são medidas distintas. Este protótipo mede RMS e pico de amostra. A margem é uma proteção baseada no trecho medido e pode mudar com trechos mais fortes, processamento posterior ou a soma de pistas no bus.

## Referências de pico por instrumento

No editor **Track**, selecione o instrumento em **Referência de pico por instrumento**. O alvo e a faixa aparecem em dBFS de pico de amostra. Na calibração técnica por instrumento, o **ANALYZER** calcula o trim individual pela diferença entre o alvo e o pico medido antes do trim. Sem instrumento selecionado, usa a referência RMS ajustável. No Balanço musical, a faixa orienta a sugestão das pistas secundárias. Os limites de trim e headroom têm prioridade. Em Balanço musical, a pista de foco preserva sua função de âncora. A calibração em grupo com ganho comum prioriza preservar os níveis relativos. No HUB, o indicador de −6 dBFS é uma referência visual para o bus pré-master.

Para calibrar bumbo e outras fontes com pausas, reproduza vários golpes dentro da janela do **Auto Gain**. A calibração técnica por pico considera os picos capturados mesmo que haja uma pausa no final. No Studio One, os inserts da pista medem e ajustam antes do fader. Mantenha o fader em 0 dB se quiser conferir no canal o mesmo nível da saída do insert, desde que não haja outro processamento depois dele. Para calibrar um sinal baixado antes do insert, ajuste o ganho do evento de áudio ou o nível de saída do instrumento virtual.

## Hierarquia de mix

Em modo **Track**, deixe o papel em **Auto** ou **Acompanhamento** e escolha o tipo de acompanhamento. No **HUB**, escolha a pista **Principal** e clique em **ANALYZER**. O plugin usa o nível atual da Principal como âncora relativa em RMS. Estes são pontos de partida do MixPilot, ajustados pelos limites de trim e pico de amostra:

| Papel | Diferença nominal em relação à Principal |
| --- | ---: |
| Principal | 0 dB |
| Bateria | −2 dB |
| Baixo | −3 dB |
| Acompanhamento padrão | −5 dB |
| Voz de apoio | −6 dB |
| Harmonia | −7 dB |
| Percussão auxiliar | −9 dB |
| Ambiência / FX | −12 dB |

Os valores da tabela são escolhas de mixagem. A recomendação [ITU-R BS.1770](https://www.itu.int/rec/R-REC-BS.1770) define a medição de loudness e true peak, e a [EBU R128](https://tech.ebu.ch/publications/r128) aplica loudness ao programa de áudio. Essas referências tratam de outras medições. O MixPilot usa RMS e pico de amostra na análise de pistas descrita acima.

## Balanço musical e análise tonal

- Para testar o **Balanço musical**, marque uma pista ativa como **Foco**. O nível atual do foco é a âncora. Os outros níveis são sugeridos por categoria e limitados pela margem. O modo **Auto** identifica predominância de graves a partir da energia espectral. A escolha do foco e a classificação das demais funções musicais continuam editáveis no Track. O protótipo ainda não identifica instrumentos, gênero ou intenção de mixagem automaticamente.
- O espectro usa FFT Hann de 2.048 amostras e energia por banda. Cada pista com sinal tem uma curva suavizada e a mesma cor de identificação na tabela. As curvas secundárias são translúcidas e a pista Principal recebe destaque. Todas somem quando o sinal deixa de ser medido. O HUB destaca sobreposição instantânea quando duas medições têm a mesma janela informada pelo host. Isso ajuda a localizar candidatos para comparação. A confirmação de mascaramento recorrente, a estimativa de confiança e a aplicação de EQ dinâmica ficam para validação com sessões e material musical reais. A sobreposição exibida não altera o áudio.
- A comunicação é em memória, com até 64 instâncias por módulo, incluindo os HUBs. Hosts que isolam cada plugin em processos distintos impedem a troca de medições. A interface de cada Track ainda mede e aplica seu trim local. Valide no Studio One com dois Tracks na mesma sessão e um HUB no master. Verifique descoberta, ganho individual e comum, Undo, sessão reaberta e sobreposição durante a reprodução. A implementação atua no trim interno do plugin. Os faders da DAW ficam sob controle do host.

## Gain staging universal

- Em modo Track, deixe **Automático pelo nome da pista** para identificar presets por nomes e apelidos comuns. Quando o nome do host é genérico ou não é reconhecido, selecione categoria e instrumento ou use a referência RMS legada. Esta classificação usa o nome recebido do host.
- **ANALYZE** captura a proposta manual para **Aplicar trim**. **AUTO GAIN** e o **ANALYZER** original medem o trecho selecionado e aplicam um trim estático após a janela. O HUB aprende as pistas Track da sessão. **FREEZE** mantém o ganho aprendido e suspende novos ciclos automáticos. O modo **Somente análise** mantém os trims aplicados.
- **Target de pico** aceita o alvo personalizado em dBFS ou **Preset**. A coluna de sugestão mostra o trim a aplicar. A janela vai de 1,2 a 20 segundos. **Sensibilidade** percorre o percentil 85 ao 100 dos picos por janela. O máximo true peak observado continua sendo usado para limitar o ganho.
- **Ganho de saída** é um ajuste manual separado, com a mesma rampa de 35 ms. Os presets de bus do catálogo são informativos para o Auto Gain e exigem aplicação manual. A mudança de nível em um bus pode depender da soma e da fase das pistas.
- A tabela estéreo separa pico de amostra (dBFS), RMS (dBFS), VU com balística aproximada e true peak estimado (dBTP) por interpolação FIR 4×. **RESET** limpa o peak hold. A medição de VU refere 0 VU a uma senoide de −18 dBFS RMS por padrão. A estimativa true peak tem uma pequena espera interna de medição e o áudio processado mantém latência zero. O limite true peak restringe o ganho proposto com base no trecho aprendido. Sinais mais fortes depois do aprendizado, automação posterior e soma no bus podem exceder a estimativa e acender o alerta.

## Visualização espectral e Tonal Balance Match

O HUB abre com **Mix Bus**. Clique na linha para visualizar uma pista, use **Ctrl+clique** ou marque a coluna **Ver** para sobrepor curvas. **Solo** mostra uma curva, **Multi** mostra as selecionadas e **Compare** usa as duas primeiras da seleção. **Reference** compara o sinal com a referência guardada. **Before/After** mostra entrada e saída reais da pista selecionada ou da instância local. **Clear** limpa o gráfico e a seleção visual. **Freeze** congela apenas a visualização. **Norm** alinha os máximos das curvas para comparar forma, sem alterar áudio. A roda do mouse sobre o gráfico aproxima as frequências ao redor do cursor. O menu **Vista** ajusta suavização e média temporal. O cursor mostra o nível da última curva desenhada na frequência indicada.

**Tonal Match** abre o painel lateral. Em **Pistas alvo**, selecione as pistas que poderão receber EQ. Essa seleção é independente da coluna **Ver** e da caixa de **Foco**. Escolha um dos perfis ou capture uma referência e use **Personalizado**. Também é possível importar CSV de duas colunas, frequência em Hz e nível em dB, separado por vírgula ou ponto e vírgula. A referência capturada/importada é salva com o estado da instância. Os perfis aplicam tendências suaves à forma medida do instrumento e consideram sua categoria e função. São pontos de partida ajustáveis.

**Sugerir EQ** prepara até três filtros por pista. A intensidade inicial é 35% e o limite inicial é ±3 dB. Edite frequência, ganho e Q se necessário. **Mascaramento** usa as janelas simultâneas das pistas em **Ver** e sugere cortes nos alvos secundários. O panorama é estimado pela energia L/R que chega ao insert. Em **Compare**, a região candidata recebe destaque. Sem posição sincronizada do host, a detecção de mascaramento não propõe ajustes.

**Ouvir B** ativa uma prévia temporária. Volte a **A** para ouvir o EQ persistido antes da proposta. **Aplicar EQ** grava e torna os controles disponíveis ao host. **Cancelar** descarta a proposta, **Desfazer EQ** restaura os parâmetros anteriores e **Bypass EQ** desativa o estágio tonal. A compensação aproxima a energia medida e depende do sinal. O EQ de ganho zero passa o sinal sem atraso e o EQ ativo usa filtros IIR de fase mínima. A transição de parâmetros usa uma mistura de 35 ms.

A FFT estéreo de 4096, 8192 ou 16384 pontos trabalha fora da thread de áudio, com janelas Hann, fila de tamanho fixo e escalas logarítmicas. As escolhas de FFT e média feitas no HUB são enviadas às instâncias da sessão. A FFT serve à análise e não adiciona latência à saída. O HUB e os buses identificados por nome/preset são excluídos do EQ tonal; marque **Excluir do EQ** em canais de soma cujo nome ou preset não os identifique. O VST3 não recebe o mapa completo de roteamento da DAW.

## Smart Mix Prepare

Abra **Assistente** no HUB. O editor abre no modo Essencial e mantém medição, ganho e Tonal Match à vista. **Avançado** revela os presets, o ganho de saída, a janela e os medidores estéreo. **Technical Calibration** mantém a calibração por instrumento e aceita uma faixa de tolerância baseada no tipo de sinal. **Musical Balance** mantém a hierarquia liderada pelo Foco. **Preserve Existing Mix** mantém trims seguros e propõe somente atenuações necessárias para a margem. **Hybrid Intelligent** combina referência técnica, forma do sinal, mediana do RMS ativo e hierarquia musical. Na falta de um Foco explícito, o Híbrido usa a maior prioridade entre as pistas com sinal.

No **Musical Balance**, a voz ou instrumento em Foco mantém seu trim enquanto houver margem. As outras pistas só recebem atenuação, e um boost de trim aplicado antes volta no máximo a 0 dB. O aprendizado usa a mediana do RMS das janelas ativas para a âncora, o percentil 85 das janelas ativas para o acompanhamento e inclui o ganho de saída de cada Track na comparação. Use **Technical Calibration** quando quiser aproximar cada pista de uma referência, inclusive com ganho positivo.

O **Auto Gain** considera o teto de pico do instrumento e o limite de true peak. As faixas iniciais usam as referências do projeto, por exemplo −6 dBFS para bumbo, caixa e baixo e −10 dBFS para guitarra. Presets adicionais usam a margem descrita no catálogo, e um Target personalizado funciona como teto de pico. O cálculo usa o maior pico de amostra da janela mesmo com sensibilidade reduzida. Ganho de saída e reforço de EQ entram na estimativa. Se o limite de trim impedir a correção, a pista é sinalizada e a proposta insegura fica sem aplicação. Essas referências orientam um ajuste estático.

Selecione **Estilo** no HUB antes de rodar o **Auto Gain** ou o **Assistente**. **Universal** mantém o balanço-base. **Worship**, **Gospel**, **Pop**, **Pop Rock** e **Acoustic** aplicam tendências suaves às funções de bateria, baixo, harmonia, vozes de apoio e ambiência. Em Musical/Híbrido, essas tendências deslocam os alvos relativos ao Foco. Na Calibração técnica, modificam apenas o alvo automático de presets identificados, em até 0,75 dB. O alvo manual, a referência RMS, os tetos de pico, o true peak e o limite de trim têm as regras já descritas. Trocar o estilo invalida uma prévia de ganho ainda pendente. Os perfis de estilo são pontos de partida editoriais. Padrões de loudness de distribuição tratam do programa, sem definir um nível obrigatório de guitarra ou vocal por gênero.

No Track, **Prioridade** vai de 0 a 100 e influencia o peso das decisões. **Função musical** identifica a função da pista, inclusive quando ainda está em Auto. **Travar volume**, **Travar EQ** e **Referência** protegem a pista das respectivas ações de preparação. O medidor e a visualização continuam disponíveis. As recomendações partem do nível original e usam trims absolutos, de modo que uma segunda análise do mesmo trecho não acumula o mesmo ganho.

Marque **Gain**, **Tonal** e **Másc.** conforme necessário. Se o preparo incluir EQ, escolha antes as pistas na coluna **EQ** ou em **Pistas alvo** do Tonal Match. Se nenhuma foi escolhida, o Assistente prepara sugestões para os Tracks elegíveis. A análise por si só não grava EQ. Para mascaramento, escolha os espectros na coluna **Ver**. **Analisar trecho** aguarda a janela de aprendizado. **Ouvir B**, **Aplicar** e **Cancelar** controlam a prévia e a aplicação. **Desfazer** e **Refazer** revalidam o estado das pistas. O relatório mostra entrada e saída, ganho desejado, permitido e aplicado, margem, crista, atividade e as bandas sugeridas. Os alvos e a margem são verificados de novo antes de gravar os ajustes.

**Ajustar saída** atua somente no ganho de saída do HUB com base na soma efetivamente medida. A referência ajustável começa em −6 dBFS de pico de amostra. A margem true peak também limita a proposta. O estágio tonal verifica a folga da pista com base no sinal medido e nos reforços previstos. A soma entre pistas e trechos mais fortes podem mudar a margem, que deve ser reavaliada após aplicar. O plugin não introduz limitador nem tenta inferir a topologia completa da DAW.

### Graves, voz e referências

**Low End** busca sobreposições de 20 a 300 Hz nas pistas visualizadas. Quando as fontes compartilham o bin dominante sincronizado e têm um sinal central utilizável, o diagnóstico também mostra a diferença de fase. **Vocal** usa um vocal reconhecido como referência e sugere cortes no acompanhamento entre 220 Hz e 5 kHz quando há evidência de sobreposição. A detecção de ressonância usa persistência temporal e energia vizinha. Esses módulos sugerem mudanças estáticas e preservam a fase/polaridade original das pistas como parâmetros de roteamento; o EQ IIR ativo tem a resposta de fase esperada de um filtro de fase mínima.

**Importar** aceita CSV, WAV, FLAC e MP3 mono/estéreo. A análise de áudio roda em segundo plano e usa até os primeiros 120 segundos. Ela estima LUFS integrado com ponderação K e gates, além de RMS, pico, crista, balanço e correlação estéreo. A captura da DAW usa o espectro médio, RMS, pico e balanço medidos. **Biblioteca** salva curvas nomeadas como Mix, Master ou Instrumento, associadas a um gênero. Você pode carregar ou combinar referências após normalização de energia. A biblioteca fica no estado da instância. Os dez perfis iniciais são tendências **provisórias**, e **Personalizado** usa a curva de áudio capturada, importada ou combinada.

A validação de DSP inclui FFT, resposta do EQ, THD de regime, fase entre canais, transição, transparência, compensação, margem, não-acúmulo de ganho e estimativa de carga. Os testes de integração exercitam o caminho HUB/TRACK, seleção visual independente, prévia, aprovação, restauração de EQ, importação WAV e 32 instâncias. O workflow Windows exige pluginval nível 10 antes de publicar.
