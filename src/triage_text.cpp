#include "triage_text.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace pc {
namespace {

using json = nlohmann::json;

enum class Lang { Ja, En, Es };

Lang ParseLanguage(const std::string& language) {
    if (language == "ja" || language.rfind("ja-", 0) == 0) return Lang::Ja;
    if (language == "es" || language.rfind("es-", 0) == 0) return Lang::Es;
    return Lang::En;
}

const char* Pick(Lang lang, const char* ja, const char* en, const char* es) {
    return lang == Lang::Ja ? ja : lang == Lang::Es ? es : en;
}

std::string JsonValue(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "none";
    if (value.is_array()) {
        std::string out;
        for (const auto& item : value) {
            if (!out.empty()) out += ", ";
            out += JsonValue(item);
        }
        return out;
    }
    return value.dump();
}

std::string Replace(std::string text, const std::string& key, const std::string& value) {
    const std::string needle = "{" + key + "}";
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        text.replace(at, needle.size(), value);
        at += value.size();
    }
    return text;
}

std::string Get(const json& value, const char* key, const std::string& fallback = "") {
    const auto it = value.find(key);
    return it != value.end() && !it->is_null() ? JsonValue(*it) : fallback;
}

long long Integer(const json& value, const char* key, long long fallback = 0) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return fallback;
    try { return it->get<long long>(); } catch (...) { return fallback; }
}

std::string ValueText(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) return JsonValue(value);
    return value.is_null() ? std::string() : value.dump();
}

std::string DurationUnit(Lang lang, long long amount, bool minute) {
    if (lang == Lang::Ja)
        return std::to_string(amount) + (minute ? " 分" : " 秒");
    return std::to_string(amount) + (minute ? " m" : " s");
}

std::string TranslateToken(Lang lang, const std::string& type, const std::string& value) {
    if (value.empty()) return "";
    if (type == "class") {
        if (value == "timeout" || value == "timed_out") return Pick(lang, "タイムアウト", "timeout", "tiempo agotado");
        if (value == "idle_stall" || value == "hang" || value == "hung") return Pick(lang, "ハング", "hang", "bloqueo");
        if (value == "crash") return Pick(lang, "クラッシュ", "crash", "fallo inesperado");
        if (value == "nonzero_exit" || value == "exit_nonzero") return Pick(lang, "終了コード異常", "non-zero exit", "código de salida distinto de cero");
        if (value == "check_failed" || value == "success_check_failed") return Pick(lang, "成功条件不成立", "success check failed", "falló la comprobación de éxito");
        if (value == "auth_error") return Pick(lang, "認証エラー", "authentication error", "error de autenticación");
        if (value == "rate_limited") return Pick(lang, "レート制限", "rate limited", "límite de solicitudes");
        if (value == "network_error") return Pick(lang, "ネットワークエラー", "network error", "error de red");
        if (value == "launch_failed" || value == "launch_error") return Pick(lang, "起動失敗", "launch error", "error de inicio");
        if (value == "permission_denied") return Pick(lang, "権限不足", "permission denied", "permiso denegado");
        if (value == "turn_or_context_limit") return Pick(lang, "ターン / コンテキスト上限", "turn or context limit", "límite de turnos o contexto");
        if (value == "tool_error_loop") return Pick(lang, "ツールエラーの反復", "tool error loop", "ciclo de errores de herramienta");
        if (value == "incomplete") return Pick(lang, "未完了", "incomplete", "incompleto");
        if (value == "killed") return Pick(lang, "強制終了", "killed", "finalizado a la fuerza");
        if (value == "dependency_blocked") return Pick(lang, "依存関係で停止", "dependency blocked", "bloqueado por dependencia");
        if (value == "resource_conflict") return Pick(lang, "リソース競合", "resource conflict", "conflicto de recursos");
        if (value == "cancelled") return Pick(lang, "キャンセル", "cancelled", "cancelado");
        if (value == "unknown") return Pick(lang, "不明", "unknown", "desconocido");
    } else if (type == "attemptReason") {
        if (value == "timed_out") return Pick(lang, "タイムアウト", "timed out", "tiempo agotado");
        if (value == "idle_timed_out") return Pick(lang, "アイドルタイムアウト", "idle timeout", "tiempo de inactividad agotado");
        if (value == "exit_code") return Pick(lang, "終了コード", "exit code", "código de salida");
        if (value == "check_failed") return Pick(lang, "成功判定失敗", "success check failed", "falló la comprobación de éxito");
        if (value == "launch_failed" || value == "launch_error") return Pick(lang, "起動失敗", "launch failed", "falló el inicio");
        if (value == "cancelled") return Pick(lang, "キャンセル", "cancelled", "cancelado");
        if (value == "killed") return Pick(lang, "強制終了", "killed", "finalizado a la fuerza");
        if (value == "failed") return Pick(lang, "失敗", "failed", "fallido");
        if (value == "succeeded") return Pick(lang, "成功", "succeeded", "correcto");
        if (value == "ok") return Pick(lang, "成功", "completed successfully", "completado correctamente");
        if (value == "interrupted") return Pick(lang, "中断", "interrupted", "interrumpido");
        if (value == "completed") return Pick(lang, "完了", "completed", "completado");
    } else if (type == "timeoutPattern") {
        if (value == "progressing") return Pick(lang, "進行中に打ち切り", "still progressing", "seguía avanzando");
        if (value == "stalled") return Pick(lang, "停止", "stalled", "detenido");
        if (value == "repeated") return Pick(lang, "繰り返し", "repeated", "repetido");
        if (value == "unknown") return Pick(lang, "不明", "unknown", "desconocido");
    } else if (type == "confidence") {
        if (value == "high") return Pick(lang, "高", "high", "alta");
        if (value == "medium") return Pick(lang, "中", "medium", "media");
        if (value == "low") return Pick(lang, "低", "low", "baja");
    } else if (type == "cardState") {
        if (value == "pending") return Pick(lang, "判断待ち", "pending", "pendiente");
        if (value == "acted") return Pick(lang, "処置済み", "acted", "tratado");
        if (value == "ignored") return Pick(lang, "無視", "ignored", "ignorado");
        if (value == "resolved") return Pick(lang, "自然解消", "resolved", "resuelto");
    } else if (type == "lifecycle") {
        if (value == "pending") return Pick(lang, "判断待ち", "Pending", "Pendiente");
        if (value == "handling") return Pick(lang, "対応中", "Handling", "En curso");
        if (value == "running") return Pick(lang, "再実行中", "Rerunning", "Reintentando");
        if (value == "waiting_branch") return Pick(lang, "分岐の完了待ち", "Waiting for branch", "Esperando rama");
        if (value == "waiting_next") return Pick(lang, "再失敗 → 次のカードへ", "Failed again → next card", "Falló de nuevo → siguiente tarjeta");
        if (value == "follow_up") return Pick(lang, "要フォロー", "Follow-up needed", "Requiere seguimiento");
        if (value == "done") return Pick(lang, "完了", "Done", "Completado");
    } else if (type == "outcome") {
        if (value == "succeeded") return Pick(lang, "成功", "Succeeded", "Correcto");
        if (value == "failed_again") return Pick(lang, "再失敗", "Failed again", "Falló de nuevo");
        if (value == "rerunning") return Pick(lang, "再実行中", "Rerunning", "Reintentando");
        if (value == "not_rerun") return Pick(lang, "再実行されていない", "Not rerun", "No reintentado");
        if (value == "cancelled") return Pick(lang, "キャンセル", "Cancelled", "Cancelado");
        if (value == "gone") return Pick(lang, "対象なし", "Gone", "Desaparecido");
    } else if (type == "jobState") {
        if (value == "queued" || value == "waiting") return Pick(lang, "待機中", "waiting", "en espera");
        if (value == "running") return Pick(lang, "実行中", "running", "en ejecución");
        if (value == "succeeded" || value == "completed") return Pick(lang, "成功", "succeeded", "correcto");
        if (value == "failed") return Pick(lang, "失敗", "failed", "fallido");
        if (value == "blocked") return Pick(lang, "ブロック中", "blocked", "bloqueado");
        if (value == "cancelled") return Pick(lang, "キャンセル", "cancelled", "cancelado");
        if (value == "timed_out") return Pick(lang, "タイムアウト", "timed out", "tiempo agotado");
        if (value == "idle_timed_out") return Pick(lang, "アイドルタイムアウト", "idle timeout", "tiempo de inactividad agotado");
        if (value == "skipped") return Pick(lang, "スキップ", "skipped", "omitido");
    } else if (type == "kind") {
        if (value == "job_failure") return Pick(lang, "ジョブ失敗", "Job failure", "Fallo de tarea");
        if (value == "bt_escalation_failure") return Pick(lang, "エスカレーション失敗", "Escalation failure", "Fallo de escalación");
        if (value == "process_failure") return Pick(lang, "プロセス失敗", "Process failure", "Fallo de proceso");
        if (value == "timing_divergence") return Pick(lang, "見積もり乖離", "Timing divergence", "Desviación de tiempo");
        if (value == "resource_wait") return Pick(lang, "リソース待ち", "Resource wait", "Espera de recurso");
    } else if (type == "section") {
        if (value == "cause") return Pick(lang, "原因", "Cause", "Causa");
        if (value == "background") return Pick(lang, "背景", "Background", "Contexto");
        if (value == "environment") return Pick(lang, "環境", "Environment", "Entorno");
        if (value == "conditions") return Pick(lang, "条件", "Conditions", "Condiciones");
        if (value == "reason") return Pick(lang, "理由", "Reason", "Motivo");
    } else if (type == "noneLocal") {
        if (value == "none" || value == "null" || value == "") return Pick(lang, "なし", "none", "ninguno");
        if (value == "local") return Pick(lang, "ローカル", "local", "local");
    } else if (type == "mode") {
        if (value == "pipe") return Pick(lang, "パイプ", "pipe", "tubería");
        if (value == "pty") return Pick(lang, "疑似端末", "pseudo-terminal", "terminal pseudo");
        if (value == "console") return Pick(lang, "コンソール", "console", "consola");
        if (value == "read") return Pick(lang, "読み取り", "read", "lectura");
        if (value == "write") return Pick(lang, "書き込み", "write", "escritura");
        if (value == "diverged_over") return Pick(lang, "見積もりより長い", "over estimate", "por encima de la estimación");
        if (value == "diverged_under") return Pick(lang, "見積もりより短い", "under estimate", "por debajo de la estimación");
    }
    return value;
}

std::string InfoPrefix(Lang lang, const std::string& key) {
    if (key == "exitCode") return Pick(lang, "終了コード: ", "Exit code: ", "Código de salida: ");
    if (key == "checkDetail") return Pick(lang, "成功判定に失敗: ", "Success check failed: ", "Falló la comprobación de éxito: ");
    if (key == "eventSequence") return Pick(lang, "イベント番号: ", "Event sequence: ", "Secuencia del evento: ");
    if (key == "jobName" || key == "processName") return Pick(lang, "名前: ", "Name: ", "Nombre: ");
    if (key == "command") return Pick(lang, "コマンド: ", "Command: ", "Comando: ");
    if (key == "queue") return Pick(lang, "キュー: ", "Queue: ", "Cola: ");
    if (key == "behaviorTree") return Pick(lang, "BT: ", "BT: ", "BT: ");
    if (key == "attempt") return Pick(lang, "試行: ", "Attempt: ", "Intento: ");
    if (key == "origin") return Pick(lang, "追加元: ", "Origin: ", "Origen: ");
    if (key == "blockedDependents") return Pick(lang, "後続で停止中: ", "Blocked dependents: ", "Dependientes bloqueados: ");
    if (key == "host") return Pick(lang, "ホスト: ", "Host: ", "Host: ");
    if (key == "cwd") return "cwd: ";
    if (key == "write") return Pick(lang, "書き込みリソース: ", "Write resources: ", "Recursos de escritura: ");
    if (key == "read") return Pick(lang, "読み取りリソース: ", "Read resources: ", "Recursos de lectura: ");
    if (key == "resourceHolder") return Pick(lang, "リソース保持者: ", "Resource holder: ", "Titular del recurso: ");
    if (key == "concurrencyAtFailure") return Pick(lang, "失敗時の同時実行: ", "Concurrent jobs at failure: ", "Tareas simultáneas al fallar: ");
    if (key == "waitingSince") return Pick(lang, "待機開始: ", "Waiting since: ", "En espera desde: ");
    if (key == "timeoutSec") return Pick(lang, "タイムアウト: ", "Timeout: ", "Tiempo límite: ");
    if (key == "idleTimeoutSec") return Pick(lang, "アイドルタイムアウト: ", "Idle timeout: ", "Tiempo límite de inactividad: ");
    if (key == "attemptDurationSec" || key == "durationSec" || key == "actualDurationSec") return Pick(lang, "所要時間: ", "Duration: ", "Duración: ");
    if (key == "estimateSec") return Pick(lang, "見積もり: ", "Estimate: ", "Estimación: ");
    if (key == "elapsedEstimateRatio" || key == "ratio") return Pick(lang, "経過/見積もり比: ", "Elapsed/estimate ratio: ", "Proporción transcurrido/estimado: ");
    if (key == "idle") return Pick(lang, "アイドル監視: ", "Idle monitoring: ", "Supervisión de inactividad: ");
    if (key == "successExitCode") return Pick(lang, "成功時の終了コード: ", "Success exit code: ", "Código de salida correcto: ");
    if (key == "successFilesNewer") return Pick(lang, "更新を確認するファイル: ", "Files required to be newer: ", "Archivos que deben actualizarse: ");
    if (key == "successOutputRegex") return Pick(lang, "成功出力の正規表現: ", "Success output regex: ", "Expresión regular de salida correcta: ");
    if (key == "successTestReport") return Pick(lang, "テストレポート: ", "Test report: ", "Informe de pruebas: ");
    if (key == "maxAttempts") return Pick(lang, "最大試行回数: ", "Maximum attempts: ", "Intentos máximos: ");
    if (key == "retryDelaySec") return Pick(lang, "再試行間隔: ", "Retry delay: ", "Espera para reintentar: ");
    if (key == "attemptReason") return Pick(lang, "試行の終了理由: ", "Attempt ended because: ", "Motivo de fin del intento: ");
    if (key == "failureRule") return Pick(lang, "判定規則: ", "Failure rule: ", "Regla de fallo: ");
    if (key == "llmGuess") return Pick(lang, "タスクキュー LLM の見立て (参考): ", "Task queue LLM guess (reference only): ", "Predicción del LLM de la cola (solo referencia): ");
    if (key == "lastActivity") return Pick(lang, "最後の動作: ", "Last activity: ", "Última actividad: ");
    if (key == "timeoutPattern") return Pick(lang, "タイムアウトの見立て: ", "Timeout pattern: ", "Patrón de tiempo límite: ");
    if (key == "lastOutput") return Pick(lang, "最後の出力", "Last output", "Última salida");
    if (key == "attemptHistory") return Pick(lang, "試行履歴:", "Attempt history:", "Historial de intentos:");
    if (key == "eventSequence") return Pick(lang, "イベント番号: ", "Event sequence: ", "Secuencia del evento: ");
    if (key == "attemptDurationSec") return Pick(lang, "最後の試行時間: ", "Last attempt duration: ", "Duración del último intento: ");
    if (key == "actualDurationSec") return Pick(lang, "実際の所要時間: ", "Actual duration: ", "Duración real: ");
    if (key == "durationSec") return Pick(lang, "経過時間: ", "Elapsed: ", "Transcurrido: ");
    if (key == "escalationTimeoutSec") return Pick(lang, "エスカレーションのタイムアウト: ", "Escalation timeout: ", "Tiempo límite de escalación: ");
    if (key == "escalationFailure") return Pick(lang, "BT エスカレーション失敗: ", "Behavior tree escalation failed: ", "Falló la escalación del árbol: ");
    if (key == "treeId") return "BT: ";
    if (key == "treeName") return Pick(lang, "ツリー: ", "Tree: ", "Árbol: ");
    if (key == "treeState") return Pick(lang, "ツリーの状態: ", "Tree state: ", "Estado del árbol: ");
    if (key == "nodeId") return Pick(lang, "ノード: ", "Node: ", "Nodo: ");
    if (key == "question") return Pick(lang, "質問: ", "Question: ", "Pregunta: ");
    if (key == "options") return Pick(lang, "選択肢: ", "Options: ", "Opciones: ");
    if (key == "guidance") return Pick(lang, "ガイダンス: ", "Guidance: ", "Orientación: ");
    if (key == "relatedJobs") return Pick(lang, "関連ジョブ: ", "Related jobs: ", "Tareas relacionadas: ");
    if (key == "escalationTimeoutSec") return Pick(lang, "エスカレーションのタイムアウト: ", "Escalation timeout: ", "Tiempo límite de escalación: ");
    if (key == "endState") return Pick(lang, "状態: ", "State: ", "Estado: ");
    if (key == "logFile") return Pick(lang, "ログ: ", "Log: ", "Registro: ");
    if (key == "startedAt") return Pick(lang, "開始時刻: ", "Started at: ", "Inicio: ");
    if (key == "mode") return Pick(lang, "実行モード: ", "Mode: ", "Modo: ");
    if (key == "processEndReason") return Pick(lang, "プロセス終了理由: ", "Process ended in state: ", "El proceso terminó en estado: ");
    if (key == "timingAccuracy") return Pick(lang, "時間の精度: ", "Timing accuracy: ", "Precisión del tiempo: ");
    if (key == "divergenceThreshold") return Pick(lang, "乖離のしきい値: ", "Divergence threshold: ", "Umbral de desviación: ");
    if (key == "recalibrateCount") return Pick(lang, "再見積もりで変わる未完了ジョブ数: ", "Unfinished jobs affected by recalibration: ", "Tareas sin terminar afectadas por el reajuste: ");
    if (key == "timingAdvice") return "";
    if (key == "resourceWait") return Pick(lang, "待機中のリソース: ", "Waiting for resource: ", "Esperando el recurso: ");
    if (key == "resourceConflict") return Pick(lang, "ロック競合: ", "Lock conflict: ", "Conflicto de bloqueo: ");
    return "";
}

std::string FormatItems(const json& value) {
    if (!value.is_array()) return JsonValue(value);
    std::string out;
    for (const auto& entry : value) {
        if (!out.empty()) out += ", ";
        out += JsonValue(entry);
    }
    return out;
}

} // namespace

std::string TriageLabel(const std::string& language, const std::string& type,
                        const std::string& value, const std::string& fallback) {
    std::string translated = TranslateToken(ParseLanguage(language), type, value);
    return translated == value && !fallback.empty() ? fallback : translated;
}

std::string TriageFormatDuration(const std::string& language, long long seconds) {
    const Lang lang = ParseLanguage(language);
    if (seconds < 0) return TriageLabel(language, "noneLocal", "none");
    if (seconds < 60) return DurationUnit(lang, seconds, false);
    const long long minutes = seconds / 60;
    const long long remainder = seconds % 60;
    std::string result = DurationUnit(lang, minutes, true);
    if (remainder) result += lang == Lang::Ja ? " " : " ", result += DurationUnit(lang, remainder, false);
    return result;
}

std::string TriageFormatRationale(const std::string& language, const std::string& key,
                                  const json& params, const std::string& fallback) {
    const Lang lang = ParseLanguage(language);
    std::string text;
    if (key == "timeoutProgressing") {
        text = Pick(lang, "タイムアウト ({timeoutSec}) の直前まで進行していました。時間を延ばして再試行してください。", "The job was still making progress near its {timeoutSec} timeout; retry with more time.", "El trabajo seguía avanzando cerca del límite de {timeoutSec}; reinténtelo con más tiempo.");
        text = Replace(std::move(text), "timeoutSec", TriageFormatDuration(language, Integer(params, "timeoutSec")));
    }
    else if (key == "timeoutStalled") text = Pick(lang, "タイムアウトよりかなり前に進行が止まっていました。まず原因を調べてください。", "The job had stopped making progress well before the timeout; investigate first.", "El trabajo dejó de avanzar mucho antes del límite; investigue primero.");
    else if (key == "timeoutRepeated") text = Pick(lang, "連続してタイムアウトしました。再試行の前に情報を集めてください。", "Consecutive attempts timed out; gather more information before retrying.", "Varios intentos consecutivos agotaron el tiempo; reúna más información antes de reintentar.");
    else if (key == "timeoutUnknown") text = Pick(lang, "タイムアウト時の進行状況は不明です。時間を延ばすのは慎重な選択肢です。", "Timeout progress is unknown; a longer timeout is a cautious possibility.", "Se desconoce el progreso al agotarse el tiempo; ampliar el límite es una opción prudente.");
    else if (key == "idleStall") text = Pick(lang, "動作がないまま停止しました。まず原因を調べてください。", "The job stalled without activity; investigate the cause first.", "El trabajo se detuvo sin actividad; investigue primero la causa.");
    else if (key == "retryAfter") {
        text = Pick(lang, "レート制限中です。{retryAt} 以降に再試行してください。", "The failure is rate-limited; retry after {retryAt}.", "Hay un límite de solicitudes; reintente después de {retryAt}.");
        text = Replace(std::move(text), "retryAt", Get(params, "retryAt"));
    } else if (key == "raiseTimeout") text = Pick(lang, "失敗分類器はタイムアウトの延長を推奨しています。", "The failure classifier recommends raising the timeout.", "El clasificador recomienda ampliar el tiempo límite.");
    else if (key == "fixSpecification") text = Pick(lang, "失敗分類器はジョブの仕様修正を推奨しています。", "The failure classifier recommends correcting the job specification.", "El clasificador recomienda corregir la especificación de la tarea.");
    else if (key == "transientRetry") {
        text = Pick(lang, "失敗 ({class}) は一時的なようです。変更せずに再試行すれば成功する可能性があります。", "The {class} failure looks transient; an unchanged retry is likely to pass.", "El fallo de {class} parece temporal; es probable que un reintento sin cambios funcione.");
    } else if (key == "retryCheckCause") text = Pick(lang, "失敗 ({class}) は再試行が提案されていますが、同じ実行では再び失敗する可能性があります。先に原因を確認してください。", "The classifier suggests retrying the {class} failure, but an unchanged rerun may fail the same way; check the cause first.", "El clasificador sugiere reintentar el fallo de {class}, pero podría repetirse; revise primero la causa.");
    else if (key == "noRecommendation") text = Pick(lang, "安全で決定論的な推奨はありません。", "The failure does not have a safe, deterministic recommendation.", "No hay una recomendación segura y determinista para este fallo.");
    else if (key == "btEscalationFailure") text = Pick(lang, "失敗した判断要求を人が確認してください。", "A human should inspect the failed decision request.", "Una persona debe revisar la solicitud de decisión fallida.");
    else if (key == "processFailure") text = Pick(lang, "通常のプロセスを再起動すべきか Hancho は判断できません。", "Hancho cannot determine whether a plain process should be restarted.", "Hancho no puede determinar si debe reiniciarse un proceso normal.");
    else if (key == "timingDivergence") text = Pick(lang, "実績時間を使って、同種・同キューの未完了ジョブの見積もりを調整してください。", "Use the measured duration to recalibrate unfinished jobs of the same kind and queue.", "Use la duración medida para reajustar tareas sin terminar del mismo tipo y cola.");
    else if (key == "resourceWait") text = Pick(lang, "待機を記録しながら、リソーススケジューラに処理を続けさせてください。", "Let the resource scheduler continue while recording the wait.", "Deje que el planificador de recursos continúe mientras registra la espera.");
    else return fallback;
    if (key == "transientRetry" || key == "retryCheckCause")
        text = Replace(std::move(text), "class", TriageLabel(language, "class", Get(params, "class")));
    return text;
}

std::string TriageCandidateLabel(const std::string& language, const std::string& id,
                                 const std::string& fallback) {
    const Lang lang = ParseLanguage(language);
    if (id == "retry") return Pick(lang, "そのまま再実行", "Retry unchanged", "Reintentar sin cambios");
    if (id == "retry_raise_timeout") return Pick(lang, "タイムアウトを延長して再実行", "Retry with longer timeout", "Reintentar con más tiempo");
    if (id == "fix_spec") return Pick(lang, "仕様を修正", "Fix specification", "Corregir especificación");
    if (id == "cancel" || id == "bt_cancel") return Pick(lang, "キャンセル", "Cancel", "Cancelar");
    if (id == "ignore") return Pick(lang, "無視", "Ignore", "Ignorar");
    if (id == "investigate") return Pick(lang, "原因を調査", "Investigate", "Investigar");
    if (id == "bt_retry") return Pick(lang, "ツリーを再試行", "Retry tree", "Reintentar árbol");
    if (id == "bt_resume") return Pick(lang, "ツリーを再開", "Resume tree", "Reanudar árbol");
    if (id == "restart") return Pick(lang, "再起動", "Restart", "Reiniciar");
    if (id == "ack") return Pick(lang, "確認済みにする", "Acknowledge", "Confirmar");
    if (id == "recalibrate") return Pick(lang, "見積もりを調整", "Recalibrate estimate", "Reajustar estimación");
    if (id == "wait") return Pick(lang, "待機", "Wait", "Esperar");
    if (id == "show_holder") return Pick(lang, "保持ジョブを表示", "Show resource holder", "Mostrar titular del recurso");
    return fallback.empty() ? id : fallback;
}

std::string TriageCandidateEffect(const std::string& language, const std::string& id,
                                  const std::string& fallback) {
    const Lang lang = ParseLanguage(language);
    if (id == "retry") return Pick(lang, "同じ設定でジョブを再実行します。", "Reruns the job with the same settings.", "Reintenta la tarea con la misma configuración.");
    if (id == "retry_raise_timeout") return Pick(lang, "タイムアウトを延長してジョブを再実行します。", "Reruns the job with a longer timeout.", "Reintenta la tarea con más tiempo.");
    if (id == "fix_spec") return Pick(lang, "Supervisor に仕様の修正を依頼します。", "Hand the specification to a supervisor to fix.", "Entrega la especificación a un supervisor para corregirla.");
    if (id == "cancel" || id == "bt_cancel") return Pick(lang, "対象をキャンセルします。", "Cancels the target.", "Cancela el objetivo.");
    if (id == "ignore") return Pick(lang, "理由を記録して対応不要にします。", "Records that no action is needed, with a reason.", "Registra que no se requiere acción, con un motivo.");
    if (id == "investigate") return Pick(lang, "ログを開いて原因を調べます。", "Opens the logs for investigation.", "Abre los registros para investigar.");
    if (id == "bt_retry") return Pick(lang, "失敗した BT を再試行します。", "Retries the failed behavior tree.", "Reintenta el árbol de comportamiento fallido.");
    if (id == "bt_resume") return Pick(lang, "回答を指定して BT を再開します。", "Resumes the behavior tree with an answer.", "Reanuda el árbol con una respuesta.");
    if (id == "restart") return Pick(lang, "同じコマンドでプロセスを再起動します。", "Restarts the process with the same command.", "Reinicia el proceso con el mismo comando.");
    if (id == "ack") return Pick(lang, "確認済みとして記録します。", "Marks the process as acknowledged.", "Marca el proceso como confirmado.");
    if (id == "recalibrate") return Pick(lang, "同種の未完了ジョブの見積もりを調整します。", "Adjusts estimates for unfinished jobs of the same kind.", "Ajusta las estimaciones de tareas similares sin terminar.");
    if (id == "wait") return Pick(lang, "スケジューラに待機を続けさせます。", "Leaves the job waiting for the scheduler.", "Deja que la tarea siga esperando al planificador.");
    if (id == "show_holder") return Pick(lang, "リソースを保持しているジョブを表示します。", "Shows the job holding the resource.", "Muestra la tarea que retiene el recurso.");
    return fallback;
}

std::vector<std::string> TriageFormatInfoRows(const std::string& language, const json& item) {
    if (item.is_string()) return {item.get<std::string>()};
    if (!item.is_object()) return {};
    const auto keyIt = item.find("k");
    const auto valueIt = item.find("v");
    const std::string text = Get(item, "text");
    if (keyIt == item.end() || !keyIt->is_string() || valueIt == item.end())
        return text.empty() ? std::vector<std::string>() : std::vector<std::string>{text};
    const std::string key = keyIt->get<std::string>();
    const json& value = *valueIt;
    const Lang lang = ParseLanguage(language);
    auto prefix = [&](const std::string& en) { return InfoPrefix(lang, en); };
    auto val = [&]() { return ValueText(value); };
    auto duration = [&]() -> std::string {
        try { return TriageFormatDuration(language, value.get<long long>()); }
        catch (...) { return val(); }
    };

    if (key == "summary" || key == "jobSummary" || key == "timingAdvice" || key == "treeFailureReason")
        return val().empty() ? std::vector<std::string>() : std::vector<std::string>{val()};
    if (key == "failureClass" && value.is_object()) {
        return {Pick(lang, "失敗分類: ", "Failure class: ", "Clase de fallo: ") +
            TriageLabel(language, "class", Get(value, "class")) + " (" +
            Pick(lang, "確信度: ", "confidence: ", "confianza: ") +
            TriageLabel(language, "confidence", Get(value, "confidence")) + ")"};
    }
    if (key == "timeoutPattern") return {Pick(lang, "タイムアウトの見立て: ", "Timeout pattern: ", "Patrón de tiempo límite: ") + TriageLabel(language, "timeoutPattern", val())};
    if (key == "evidence" && value.is_object()) {
        std::vector<std::string> rows;
        std::string location = Get(value, "file");
        if (value.contains("line") && !value["line"].is_null()) location += ":" + JsonValue(value["line"]);
        rows.push_back(Pick(lang, "証拠: ", "Evidence: ", "Evidencia: ") + location);
        const json lines = value.value("lines", json::array());
        for (const auto& line : lines) rows.push_back(JsonValue(line));
        return rows;
    }
    if (key == "lastOutput" && value.is_object()) {
        std::vector<std::string> rows;
        const std::string file = Get(value, "file");
        rows.push_back(Pick(lang, "最後の出力", "Last output", "Última salida") + (file.empty() ? "" : " — " + file));
        for (const auto& line : value.value("lines", json::array())) rows.push_back(JsonValue(line));
        return rows;
    }
    if (key == "recommendationRationale" && value.is_object()) {
        return {TriageFormatRationale(language, Get(value, "key"), value.value("params", json::object()), text)};
    }
    if (key == "attemptReason") return {prefix(key) + TriageLabel(language, "attemptReason", val())};
    if (key == "endState" || key == "processEndReason" || key == "treeState") return {prefix(key) + TriageLabel(language, "jobState", val())};
    if (key == "timingAccuracy") return {prefix(key) + TriageLabel(language, "mode", val())};
    if (key == "mode") return {prefix(key) + TriageLabel(language, "mode", val())};
    if (key == "host" && val() == "local") return {prefix(key) + TriageLabel(language, "noneLocal", "local")};
    if (key == "timeoutSec" || key == "idleTimeoutSec" || key == "attemptDurationSec" || key == "durationSec" ||
        key == "actualDurationSec" || key == "estimateSec" || key == "escalationTimeoutSec" ||
        key == "retryDelaySec" || key == "waitingSince") {
        if (key == "waitingSince" && value.is_object()) {
            return {Pick(lang, "待機開始: ", "Waiting since: ", "En espera desde: ") + Get(value, "at") +
                " (" + TriageFormatDuration(language, Integer(value, "elapsedSec")) + ")"};
        }
        return {prefix(key) + duration()};
    }
    if (key == "lastActivity" && value.is_object()) {
        return {Pick(lang, "最後の動作: ", "Last activity: ", "Última actividad: ") + Get(value, "activity") +
            " (" + Get(value, "target") + "), " + TriageFormatDuration(language, Integer(value, "sinceSec")) +
            Pick(lang, "前", " ago", " antes")};
    }
    if (key == "attemptHistory" && value.is_array()) {
        std::vector<std::string> rows{Pick(lang, "試行履歴:", "Attempt history:", "Historial de intentos:")};
        for (const auto& attempt : value) {
            std::string row = "  #" + Get(attempt, "n") + " " + Get(attempt, "start");
            if (attempt.contains("durationSec")) row += " — " + TriageFormatDuration(language, Integer(attempt, "durationSec"));
            if (!Get(attempt, "reason").empty()) row += " — " + TriageLabel(language, "attemptReason", Get(attempt, "reason"));
            if (!Get(attempt, "class").empty()) row += " / " + TriageLabel(language, "class", Get(attempt, "class"));
            rows.push_back(std::move(row));
        }
        return rows;
    }
    if (key == "attempt" && value.is_object())
        return {prefix(key) + Get(value, "n") + "/" + Get(value, "maxAttempts")};
    if (key == "blockedDependents" && value.is_object())
        return {prefix(key) + Get(value, "count") + " (" + FormatItems(value.value("ids", json::array())) + ")"};
    if (key == "resourceConflict" && value.is_object()) {
        const std::string lock = TriageLabel(language, "mode", Get(value, "lockMode"));
        return {Pick(lang, "", "A ", "Un bloqueo de ") + lock +
            Pick(lang, "ロックは他のロックと競合します: ", " lock conflicts with another lock: ", " entra en conflicto con otro bloqueo: ") + Get(value, "resource")};
    }
    if (key == "timeoutSec" || key == "idleTimeoutSec") return {prefix(key) + duration()};
    std::string outputPrefix = prefix(key);
    if (key == "failureClass") return {text};
    if (key == "origin" && value.is_object()) {
        if (value.contains("label")) return {outputPrefix + JsonValue(value["label"])};
        return {outputPrefix + Get(value, "client") + " + " + Get(value, "folder")};
    }
    if (key == "behaviorTree" && value.is_object())
        return {"BT " + Get(value, "id") + (Get(value, "node").empty() ? "" : " / node " + Get(value, "node"))};
    if (key == "exitCode" && value.is_object()) {
        return {Pick(lang, "終了コード: ", "Exit code: ", "Código de salida: ") + Get(value, "actual") +
            Pick(lang, " (期待値: ", " (expected: ", " (esperado: ") + Get(value, "expected") + ")"};
    }
    if (key == "exitCode" && value.is_number()) return {prefix(key) + val()};
    if (key == "timeoutSec" || key == "attemptDurationSec" || key == "estimateSec" || key == "retryDelaySec")
        return {prefix(key) + duration()};
    if (key == "elapsedEstimateRatio" || key == "ratio" || key == "divergenceThreshold") return {prefix(key) + val()};
    if (key == "resourceHolder" && value.is_object()) {
        std::string row = outputPrefix + Get(value, "resource") + " (" +
            Get(value, "holderId", Get(value, "jobId")) + ", " +
            TriageLabel(language, "mode", Get(value, "lockMode")) + Pick(lang, "ロック", " lock", " bloqueo") + ")";
        const std::string state = Get(value, "holderState", Get(value, "state"));
        if (!state.empty()) row += " [" + TriageLabel(language, "cardState", state) + "]";
        if (value.contains("holderRunningSec")) row += ", " + TriageFormatDuration(language, Integer(value, "holderRunningSec"));
        if (value.contains("runningSec")) row += ", " + TriageFormatDuration(language, Integer(value, "runningSec"));
        return {std::move(row)};
    }
    if (key == "concurrencyAtFailure" && value.is_object()) return {outputPrefix + Get(value, "count") + " (" + FormatItems(value.value("ids", json::array())) + ")"};
    if (key == "idle" && value.is_object()) return {outputPrefix + Pick(lang, "出力 ", "output ", "salida ") + TriageFormatDuration(language, Integer(value, "output_sec")) +
        Pick(lang, ", CPU ", ", CPU ", ", CPU ") + TriageFormatDuration(language, Integer(value, "cpu_sec"))};
    if (key == "timeoutSec" || key == "idleTimeoutSec" || key == "durationSec" || key == "actualDurationSec" || key == "estimateSec" || key == "escalationTimeoutSec" || key == "retryDelaySec") return {outputPrefix + duration()};
    if (key == "host" && val().empty()) return {outputPrefix + TriageLabel(language, "noneLocal", "local")};
    if (key == "origin" && val() == "none") return {outputPrefix + TriageLabel(language, "noneLocal", "none")};
    if (outputPrefix.empty()) return {text.empty() ? val() : text};
    return {outputPrefix + FormatItems(value)};
}

std::string TriageFormatInfo(const std::string& language, const json& item) {
    const auto rows = TriageFormatInfoRows(language, item);
    std::string output;
    for (const auto& row : rows) {
        if (!output.empty()) output += "\n";
        output += row;
    }
    return output;
}

} // namespace pc
