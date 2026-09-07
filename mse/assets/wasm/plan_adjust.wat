;; ============================================================================
;; plan_adjust.wat —— R-PLAN-ADJUST-WASM:A3 五状态机调序过滤(WASM 版)
;;
;; 与 jsonlogic 种子 R-PLAN-ADJUST 同一立法(读写同一份规则的 WASM 化身):
;;   计划状态 ∈ {01,02} → pass;03/04 须退回、05 绝对禁止 → reject;
;;   键缺失(初始状态,null)→ pass(未进 A3 队列的订单处于 A2 排序阶段);
;;   形状非法(存在但非带引号两字符)→ reject。
;; ABI:宿主导入 "mse".{attr_len,attr_get,result};导出 memory + mse_eval。
;; 值按 JSON 文本传输:字符串 "01" 的字节形态是带引号的 4 字节 "\"01\""。
;; 汇编:wat2wasm plan_adjust.wat -o plan_adjust.wasm(产物随仓库提交,
;; 构建期不依赖 wabt)。
;; ============================================================================
(module
  (import "mse" "attr_len" (func $attr_len (param i32 i32) (result i32)))
  (import "mse" "attr_get" (func $attr_get (param i32 i32 i32 i32) (result i32)))
  (import "mse" "result"   (func $result (param i32 i32 i32 i32)))

  (memory (export "memory") 1)

  ;; ---- 数据布局 ----
  (data (i32.const 0)  "计划状态")          ;; 键,UTF-8 12 字节
  (data (i32.const 16) "\"01\"")            ;; 允许值 1(JSON 文本,4 字节)
  (data (i32.const 24) "\"02\"")            ;; 允许值 2
  (data (i32.const 32) "pass")              ;; kind: pass
  (data (i32.const 40) "reject")            ;; kind: reject(6 字节)
  (data (i32.const 48) "R-PLAN-ADJUST-WASM:03/04 须退回,05 绝对禁止") ;; 拒绝理由
  ;; i32.const 256 起为值缓冲区(容量 64)

  ;; 拒绝结论(状态未知 / 03 / 04 / 05 统一走这里)
  (func $reject
    (call $result (i32.const 40) (i32.const 6) (i32.const 48) (i32.const 50)))

  ;; 缓冲区 4 字节与 $p 处 4 字节相等 → 1
  (func $eq4 (param $p i32) (result i32)
    (i32.and
      (i32.and
        (i32.eq (i32.load8_u (i32.const 256))
                (i32.load8_u (local.get $p)))
        (i32.eq (i32.load8_u (i32.const 257))
                (i32.load8_u (i32.add (local.get $p) (i32.const 1)))))
      (i32.and
        (i32.eq (i32.load8_u (i32.const 258))
                (i32.load8_u (i32.add (local.get $p) (i32.const 2))))
        (i32.eq (i32.load8_u (i32.const 259))
                (i32.load8_u (i32.add (local.get $p) (i32.const 3)))))))

  (func (export "mse_eval")
    (local $len i32)
    ;; 键缺失(初始状态,attr_len 返回 -1)→ pass;存在但非 4 字节 → reject
    (local.set $len (call $attr_len (i32.const 0) (i32.const 12)))
    (if (i32.eq (local.get $len) (i32.const -1))
      (then (call $result (i32.const 32) (i32.const 4) (i32.const 0) (i32.const 0))
            (return)))
    (if (i32.ne (local.get $len) (i32.const 4))
      (then (call $reject) (return)))
    ;; 取属性值 JSON 文本到缓冲区;长度不符(含 -2 截断)→ reject
    (local.set $len (call $attr_get (i32.const 0) (i32.const 12)
                                    (i32.const 256) (i32.const 64)))
    (if (i32.ne (local.get $len) (i32.const 4))
      (then (call $reject) (return)))
    ;; "01"/"02" → pass,其余 → reject
    (if (i32.or (call $eq4 (i32.const 16)) (call $eq4 (i32.const 24)))
      (then (call $result (i32.const 32) (i32.const 4) (i32.const 0) (i32.const 0)))
      (else (call $reject))))
)
