param(
    [string]$TesterRoot = '../Mini-DCM/firmware/targets/F407_Tester'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$integrationRoot = Join-Path $projectRoot 'integration\minidcm_tester'
$backupRoot = Join-Path $projectRoot ('artifacts\backups\minidcm_tester_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))

function Replace-Exact {
    param(
        [string]$Path,
        [string]$Old,
        [string]$New
    )
    $text = [IO.File]::ReadAllText($Path)
    if ($text.Contains($New)) {
        return
    }
    if (-not $text.Contains($Old)) {
        throw "Expected patch anchor not found: $Path"
    }
    [IO.File]::WriteAllText($Path, $text.Replace($Old, $New), [Text.UTF8Encoding]::new($false))
}

$required = @(
    (Join-Path $TesterRoot 'Src\main.c'),
    (Join-Path $TesterRoot 'Src\minidcm_app.c'),
    (Join-Path $TesterRoot 'Src\stm32f4xx_it.c'),
    (Join-Path $TesterRoot 'Inc\main.h'),
    (Join-Path $TesterRoot 'Inc\stm32f4xx_it.h'),
    (Join-Path $TesterRoot 'Inc\minidcm_uart_link.h')
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Tester source: $path"
    }
}

New-Item -ItemType Directory -Path $backupRoot | Out-Null
foreach ($path in $required) {
    Copy-Item -LiteralPath $path -Destination (Join-Path $backupRoot (Split-Path -Leaf $path))
}

Copy-Item -LiteralPath (Join-Path $integrationRoot 'Inc\trace_link.h') -Destination (Join-Path $TesterRoot 'Inc\trace_link.h') -Force
Copy-Item -LiteralPath (Join-Path $integrationRoot 'Src\trace_link.c') -Destination (Join-Path $TesterRoot 'Src\trace_link.c') -Force

$uartHeader = Join-Path $TesterRoot 'Inc\minidcm_uart_link.h'
Replace-Exact $uartHeader `
    '    MINIDCM_UART_CMD_CAN_TX_BURST = 0x13u,' `
    "    MINIDCM_UART_CMD_CAN_TX_BURST = 0x13u,`r`n    MINIDCM_UART_CMD_TRACE_STATS = 0x14u,`r`n    MINIDCM_UART_CMD_TRACE_BAD_CRC = 0x15u,"

$app = Join-Path $TesterRoot 'Src\minidcm_app.c'
Replace-Exact $app `
    '#include "minidcm_uart_link.h"' `
    "#include `"minidcm_uart_link.h`"`r`n#include `"trace_link.h`""
Replace-Exact $app `
    'static uint32_t s_can_tx_enqueue_count;' `
    "static uint32_t s_can_tx_enqueue_count;`r`nstatic uint32_t s_trace_last_can_error_count;`r`nstatic uint16_t s_trace_last_tx_reject_count;`r`nstatic uint16_t s_trace_last_rx_reject_count;"
Replace-Exact $app `
    '        s_can_tx_tail = (uint8_t)((s_can_tx_tail + 1u) % MINIDCM_CAN_TX_QUEUE_CAPACITY);' `
    "        (void)TraceLink_EmitCan(TRACE_SOURCE_TESTER, TRACE_EVENT_CAN_TX,`r`n                                TRACE_SEVERITY_INFO, item->std_id,`r`n                                item->dlc, item->data, HAL_GetTick());`r`n        s_can_tx_tail = (uint8_t)((s_can_tx_tail + 1u) % MINIDCM_CAN_TX_QUEUE_CAPACITY);"
Replace-Exact $app `
    "    __enable_irq();`r`n`r`n    response[0] = ready;" `
    "    __enable_irq();`r`n`r`n    if (ready != 0u) {`r`n        (void)TraceLink_EmitCan(TRACE_SOURCE_ECU, TRACE_EVENT_CAN_RX,`r`n                                TRACE_SEVERITY_INFO, id, dlc, data,`r`n                                HAL_GetTick());`r`n    }`r`n`r`n    response[0] = ready;"
Replace-Exact $app `
    'static void dispatch_frame(const MinidcmUartFrameView *frame)' `
    @'
static void handle_trace_stats(const MinidcmUartFrameView *frame)
{
    uint8_t response[41];
    TraceLinkStats stats;

    TraceLink_GetStats(&stats);
    response[0] = 1u;
    response[1] = (uint8_t)stats.state;
    response[2] = stats.ready_input;
    response[3] = stats.queue_pending;
    response[4] = stats.queue_high_water;
    memcpy(&response[5], &stats.frames_enqueued, sizeof(uint32_t));
    memcpy(&response[9], &stats.frames_sent, sizeof(uint32_t));
    memcpy(&response[13], &stats.queue_rejected, sizeof(uint32_t));
    memcpy(&response[17], &stats.dma_irq_count, sizeof(uint32_t));
    memcpy(&response[21], &stats.dma_error_count, sizeof(uint32_t));
    memcpy(&response[25], &stats.timeout_count, sizeof(uint32_t));
    memcpy(&response[29], &stats.recovery_count, sizeof(uint32_t));
    memcpy(&response[33], &stats.backpressure_count, sizeof(uint32_t));
    memcpy(&response[37], &stats.bad_crc_injected, sizeof(uint32_t));
    send_frame(frame->seq, Minidcm_UartResponseCmd(frame->cmd),
               response, sizeof(response));
}

static void handle_trace_bad_crc(const MinidcmUartFrameView *frame)
{
    uint8_t response = 0u;

    TraceLink_InjectBadCrcOnce();
    send_frame(frame->seq, Minidcm_UartResponseCmd(frame->cmd), &response, 1u);
}

static void dispatch_frame(const MinidcmUartFrameView *frame)
'@
Replace-Exact $app `
    "    if (frame->cmd == MINIDCM_UART_CMD_CAN_RX_POLL) {`r`n        handle_can_rx_poll(frame);`r`n        return;`r`n    }" `
    "    if (frame->cmd == MINIDCM_UART_CMD_CAN_RX_POLL) {`r`n        handle_can_rx_poll(frame);`r`n        return;`r`n    }`r`n`r`n    if (frame->cmd == MINIDCM_UART_CMD_TRACE_STATS) {`r`n        handle_trace_stats(frame);`r`n        return;`r`n    }`r`n`r`n    if (frame->cmd == MINIDCM_UART_CMD_TRACE_BAD_CRC) {`r`n        handle_trace_bad_crc(frame);`r`n        return;`r`n    }"
Replace-Exact $app `
    'void Minidcm_AppInit(UART_HandleTypeDef *uart, CAN_HandleTypeDef *can)' `
    @'
static void trace_publish_fault_deltas(void)
{
    uint8_t payload[4];

    if (s_can_error_count != s_trace_last_can_error_count) {
        memcpy(payload, (const void *)&s_can_error_count, sizeof(payload));
        (void)TraceLink_Emit(TRACE_SOURCE_TESTER, TRACE_EVENT_CAN_ERROR,
                             TRACE_SEVERITY_ERROR, payload, sizeof(payload),
                             HAL_GetTick());
        s_trace_last_can_error_count = s_can_error_count;
    }
    if (s_can_tx_reject_count != s_trace_last_tx_reject_count) {
        payload[0] = (uint8_t)s_can_tx_reject_count;
        payload[1] = (uint8_t)(s_can_tx_reject_count >> 8u);
        (void)TraceLink_Emit(TRACE_SOURCE_TESTER,
                             TRACE_EVENT_TX_QUEUE_OVERFLOW,
                             TRACE_SEVERITY_WARN, payload, 2u, HAL_GetTick());
        s_trace_last_tx_reject_count = s_can_tx_reject_count;
    }
    if (s_can_rx_queue_reject_count != s_trace_last_rx_reject_count) {
        payload[0] = (uint8_t)s_can_rx_queue_reject_count;
        payload[1] = (uint8_t)(s_can_rx_queue_reject_count >> 8u);
        (void)TraceLink_Emit(TRACE_SOURCE_TESTER,
                             TRACE_EVENT_RX_QUEUE_OVERFLOW,
                             TRACE_SEVERITY_WARN, payload, 2u, HAL_GetTick());
        s_trace_last_rx_reject_count = s_can_rx_queue_reject_count;
    }
}

void Minidcm_AppInit(UART_HandleTypeDef *uart, CAN_HandleTypeDef *can)
'@
Replace-Exact $app `
    "    s_can_tx_enqueue_count = 0u;`r`n    can_start_or_error();" `
    "    s_can_tx_enqueue_count = 0u;`r`n    s_trace_last_can_error_count = 0u;`r`n    s_trace_last_tx_reject_count = 0u;`r`n    s_trace_last_rx_reject_count = 0u;`r`n    (void)TraceLink_Init();`r`n    can_start_or_error();"
Replace-Exact $app `
    "    can_tx_process_queue();`r`n}" `
    "    can_tx_process_queue();`r`n    trace_publish_fault_deltas();`r`n    TraceLink_MainFunction(HAL_GetTick());`r`n}"

$mainHeader = Join-Path $TesterRoot 'Inc\main.h'
Replace-Exact $mainHeader `
    '/* USER CODE BEGIN EFP */' `
    "/* USER CODE BEGIN EFP */`r`nvoid F407_NotifyBridgeTaskFromIsr(void);"

$mainSource = Join-Path $TesterRoot 'Src\main.c'
$mainText = [IO.File]::ReadAllText($mainSource)
$staticNotify = 'static void F407_NotifyBridgeTaskFromIsr(void)'
$globalNotify = 'void F407_NotifyBridgeTaskFromIsr(void)'
if ($mainText.Contains($staticNotify)) {
    [IO.File]::WriteAllText(
        $mainSource,
        $mainText.Replace($staticNotify, $globalNotify),
        [Text.UTF8Encoding]::new($false))
} elseif (-not $mainText.Contains($globalNotify)) {
    throw "Expected ISR notification helper not found: $mainSource"
}

$irqSource = Join-Path $TesterRoot 'Src\stm32f4xx_it.c'
Replace-Exact $irqSource `
    '/* USER CODE BEGIN Includes */' `
    "/* USER CODE BEGIN Includes */`r`n#include `"trace_link.h`""
Replace-Exact $irqSource `
    "/******************************************************************************/`r`n`r`n/**`r`n  * @brief This function handles CAN1 RX0 interrupts." `
    @'
void DMA1_Stream3_IRQHandler(void)
{
  TraceLink_DmaRxIrqHandler();
  F407_NotifyBridgeTaskFromIsr();
}

void DMA1_Stream4_IRQHandler(void)
{
  TraceLink_DmaTxIrqHandler();
  F407_NotifyBridgeTaskFromIsr();
}

/******************************************************************************/

/**
  * @brief This function handles CAN1 RX0 interrupts.
'@

$irqHeader = Join-Path $TesterRoot 'Inc\stm32f4xx_it.h'
Replace-Exact $irqHeader `
    '/* USER CODE BEGIN EFP */' `
    "/* USER CODE BEGIN EFP */`r`nvoid DMA1_Stream3_IRQHandler(void);`r`nvoid DMA1_Stream4_IRQHandler(void);"

Write-Output "SYNC PASS"
Write-Output "Tester: $TesterRoot"
Write-Output "Backup: $backupRoot"
