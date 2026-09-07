/* 车间世界模型 MES · 人工验证台 (React 18 UMD + htm, 免构建) */
(function () {
  'use strict';
  const html = htm.bind(React.createElement);
  const { useState, useEffect, useRef, useCallback, useMemo } = React;

  /* ================= API ================= */
  async function api(path, opts, token) {
    opts = opts || {};
    const headers = Object.assign({}, opts.headers);
    if (token) headers['X-MSE-Token'] = token;
    if (opts.body && typeof opts.body !== 'string') {
      headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(opts.body);
    }
    const res = await fetch(path, Object.assign({}, opts, { headers }));
    let data = null;
    try { data = await res.json(); } catch (e) { /* 非 JSON */ }
    if (!res.ok) {
      const msg = (data && (data.error || data.message)) || ('HTTP ' + res.status);
      const err = new Error(msg);
      err.data = data;
      throw err;
    }
    return data;
  }

  /* ================= 工具 ================= */
  const GROUPS = { A: 'A 计划', B: 'B 物料', C: 'C AVI', D: 'D ANDON', E: 'E PMC', F: 'F 质量', G: 'G 设备', H: 'H 集成' };
  function groupOf(viewId) {
    const parts = String(viewId || '').split('-');
    const tail = parts[parts.length - 1] || '';
    const ch = tail.charAt(0).toUpperCase();
    return GROUPS[ch] || '其他';
  }
  function shortVal(v) {
    if (v === null || v === undefined) return '';
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
  }
  function writesSummary(writes) {
    if (!writes || typeof writes !== 'object') return '';
    return Object.keys(writes).map(function (oid) {
      const kv = writes[oid] || {};
      const inner = Object.keys(kv).map(function (k) { return k + '=' + shortVal(kv[k]); }).join(', ');
      return oid + ': {' + inner + '}';
    }).join(' | ');
  }
  function spaceText(space) {
    if (!space) return '';
    if (typeof space === 'string') return space;
    return space.anchor || space.raw_text || '';
  }
  /* 深链接:#view=V-PLAN-A3&entity=VIN-LBV0001&observer=计划员(&t=seq) */
  function parseHash() {
    const h = (window.location.hash || '').replace(/^#/, '');
    const out = {};
    h.split('&').forEach(function (kv) {
      if (!kv) return;
      const i = kv.indexOf('=');
      const k = i < 0 ? kv : kv.slice(0, i);
      const v = i < 0 ? '' : kv.slice(i + 1);
      try { out[decodeURIComponent(k)] = decodeURIComponent(v); } catch (e) { out[k] = v; }
    });
    return out;
  }
  function buildHash(view, entity, observer, asOf) {
    const parts = [];
    if (view) parts.push('view=' + encodeURIComponent(view));
    if (entity) parts.push('entity=' + encodeURIComponent(entity));
    if (observer) parts.push('observer=' + encodeURIComponent(observer));
    if (asOf !== '' && asOf !== undefined && asOf !== null) parts.push('t=' + encodeURIComponent(asOf));
    return parts.length ? '#' + parts.join('&') : '#';
  }
  const LAYER_NAMES = { 0: 'L0 格式', 1: 'L1 字典', 2: 'L2 信任', 3: 'L3 规则' };

  /* ================= Toast ================= */
  let toastId = 0;
  function Toasts(props) {
    return html`<div id="toasts">
      ${props.toasts.map(function (t) {
        return html`<div key=${t.id} class=${'toast ' + t.kind}>${t.text}</div>`;
      })}
    </div>`;
  }

  /* ================= 左侧视图列表 ================= */
  function ViewList(props) {
    const groups = useMemo(function () {
      const g = {};
      (props.views || []).forEach(function (v) {
        const name = groupOf(v.view_id);
        (g[name] = g[name] || []).push(v);
      });
      const order = Object.keys(GROUPS).map(function (k) { return GROUPS[k]; }).concat(['其他']);
      return order.filter(function (n) { return g[n]; }).map(function (n) {
        return { name: n, items: g[n].slice().sort(function (a, b) { return a.view_id < b.view_id ? -1 : 1; }) };
      });
    }, [props.views]);
    if (!props.views) return html`<div class="loading">加载视图…</div>`;
    return html`<div>
      ${groups.map(function (g) {
        return html`<div class="view-group" key=${g.name}>
          <div class="view-group-title">${g.name}</div>
          ${g.items.map(function (v) {
            const bad = v.status && v.status !== 'ok' && v.status !== 'active';
            return html`<div key=${v.view_id}
              class=${'view-item' + (props.selected === v.view_id ? ' active' : '')}
              onClick=${function () { props.onSelect(v.view_id); }}>
              <span class="mono">${v.view_id}</span>
              <span class=${'mode-tag' + (bad ? ' bad' : '')}>${v.render_mode || '?'}</span>
            </div>`;
          })}
        </div>`;
      })}
    </div>`;
  }

  /* ================= 终态视图 ================= */
  function TerminalTable(props) {
    const data = props.data;
    const cols = data.columns || [];
    const rows = data.rows || [];
    return html`<table class="grid">
      <thead><tr>
        <th>id</th>
        ${cols.map(function (c) { return html`<th key=${c}>${c}</th>`; })}
        <th>操作</th>
      </tr></thead>
      <tbody>
        ${rows.length === 0 && html`<tr><td colspan=${cols.length + 2} class="muted">(空)</td></tr>`}
        ${rows.map(function (r, i) {
          const btns = r.buttons || [];
          return html`<tr key=${r.id || i}>
            <td class="mono">${r.id}</td>
            ${cols.map(function (c) {
              return html`<td key=${c}><div class="cell-val" title=${shortVal(r[c])}>${shortVal(r[c])}</div></td>`;
            })}
            <td>
              ${btns.map(function (b, j) {
                const title = b.enabled ? b.type : (b.reasons || []).join('; ');
                return html`<button key=${j} class="btn row-btn" disabled=${!b.enabled} title=${title}
                  onClick=${function () { props.onAction(b.type, r.id, b); }}>${b.type}</button>`;
              })}
            </td>
          </tr>`;
        })}
      </tbody>
    </table>`;
  }

  /* ================= 流水视图 ================= */
  function FlowView(props) {
    const data = props.data;
    const rows = data.rows || [];
    return html`<table class="grid">
      <thead><tr>
        <th>#</th><th>类型</th><th>actor</th><th>occur_time</th><th>位置</th><th>writes</th><th>corrects</th><th>操作</th>
      </tr></thead>
      <tbody>
        ${rows.length === 0 && html`<tr><td colspan="8" class="muted">(空)</td></tr>`}
        ${rows.map(function (r, i) {
          const eid = 'flow-evt-' + r.event_id;
          const hl = props.highlightId === r.event_id;
          return html`<tr key=${r.event_id || i} id=${eid}
            class=${(r.is_correction ? 'correction' : '') + (hl ? ' flash-highlight' : '')}>
            <td class="mono">${r.event_id}</td>
            <td class="mono">${r.type}</td>
            <td>${r.actor || ''}</td>
            <td class="mono">${r.occur_time || ''}</td>
            <td>${spaceText(r.space)}</td>
            <td><div class="cell-val" title=${writesSummary(r.writes)}>${writesSummary(r.writes)}</div></td>
            <td>${r.corrects ? html`<a class="link" onClick=${function () { props.onJump(r.corrects); }}>#${r.corrects}</a>` : ''}</td>
            <td>
              ${(r.buttons || []).map(function (b, j) {
                // 行内动作(小号):enabled/title 逻辑同终态行按钮;点击开表单,
                // 预填 presets + id + corrects(修正类型由服务端标注)
                const title = b.enabled ? b.type : (b.reasons || []).join('; ');
                return html`<button key=${j} class="btn row-btn" disabled=${!b.enabled} title=${title}
                  onClick=${function () { props.onFlowAction(b); }}>${b.type}</button>`;
              })}
            </td>
          </tr>`;
        })}
      </tbody>
    </table>`;
  }

  /* ================= 拦截视图 ================= */
  function RejectionCards(props) {
    const rejs = props.rejections || [];
    if (rejs.length === 0) return html`<div class="muted" style=${{marginBottom:"12px"}}>当前无拦截记录</div>`;
    return html`<div class="rej-cards">
      ${rejs.map(function (rj, i) {
        const lv = rj.layer !== undefined ? rj.layer : 0;
        return html`<div key=${i} class=${'rej-card l' + lv}>
          <div class="rej-head">${LAYER_NAMES[lv] || ('L' + lv)} 拦截</div>
          <ul>${(rj.violations || []).map(function (v, j) {
            return html`<li key=${j}>${typeof v === 'string' ? v : JSON.stringify(v)}</li>`;
          })}</ul>
          ${rj.candidate && html`<pre>${JSON.stringify(rj.candidate, null, 2)}</pre>`}
        </div>`;
      })}
    </div>`;
  }
  function InterceptView(props) {
    return html`<div>
      <${RejectionCards} rejections=${props.data.rejections} />
      <${TerminalTable} data=${props.data} onAction=${props.onAction} />
    </div>`;
  }

  /* ================= 遍历视图 ================= */
  function TraverseView(props) {
    const data = props.data;
    if (!props.entity) {
      return html`<div class="hint">遍历视图需要 entity 参数:请在顶部工具条填写实体 ID 后刷新。</div>`;
    }
    const nodes = data.nodes || [];
    const edges = data.edges || [];
    // 后端遍历响应不带 columns:节点列 = 全部节点属性键的并集(除 id)
    const nodeCols = data.columns || (function () {
      const seen = {}, order = [];
      nodes.forEach(function (n) {
        Object.keys(n).forEach(function (k) {
          if (k !== 'id' && !seen[k]) { seen[k] = true; order.push(k); }
        });
      });
      return order;
    })();
    return html`<div>
      <div class="walk-section">
        <h4>节点(root: <span class="mono">${data.root || ''}</span>)</h4>
        <table class="grid">
          <thead><tr>
            <th>id</th>
            ${nodeCols.map(function (c) { return html`<th key=${c}>${c}</th>`; })}
          </tr></thead>
          <tbody>
            ${nodes.length === 0 && html`<tr><td colspan=${nodeCols.length + 1} class="muted">(空)</td></tr>`}
            ${nodes.map(function (n, i) {
              return html`<tr key=${n.id || i}>
                <td class="mono"><a class="link" onClick=${function () { props.onWalk(n.id); }}>${n.id}</a></td>
                ${nodeCols.map(function (c) {
                  return html`<td key=${c}><div class="cell-val" title=${shortVal(n[c])}>${shortVal(n[c])}</div></td>`;
                })}
              </tr>`;
            })}
          </tbody>
        </table>
      </div>
      <div class="walk-section">
        <h4>边</h4>
        <table class="grid">
          <thead><tr><th>from</th><th>key</th><th>to</th><th>源事件</th></tr></thead>
          <tbody>
            ${edges.length === 0 && html`<tr><td colspan="4" class="muted">(空)</td></tr>`}
            ${edges.map(function (e, i) {
              return html`<tr key=${i}>
                <td class="mono"><a class="link" onClick=${function () { props.onWalk(e.from); }}>${e.from}</a></td>
                <td class="mono">${e.key}</td>
                <td class="mono"><a class="link" onClick=${function () { props.onWalk(e.to); }}>${e.to}</a></td>
                <td class="mono">${e.source_event !== undefined ? '#' + e.source_event : ''}</td>
              </tr>`;
            })}
          </tbody>
        </table>
      </div>
    </div>`;
  }

  /* ================= 视图主区 ================= */
  function ViewContent(props) {
    const d = props.data;
    if (props.loading) return html`<div class="loading">加载中…</div>`;
    if (props.error) return html`<div class="err-box">${props.error}</div>`;
    if (!d) return html`<div class="muted">请选择左侧视图</div>`;
    if (d.error) return html`<div class="err-box">视图错误:${d.error}</div>`;
    const head = html`<div class="view-head">
      <h3 class="mono">${d.view_id || props.viewId}</h3>
      <span class="tag">${d.render_mode || ''}</span>
      <span class="tag">observer: ${d.observer || '默认'}</span>
      <span class="tag">as_of_seq: ${d.as_of_seq !== undefined ? d.as_of_seq : '-'}</span>
    </div>`;
    let body = null;
    if (d.render_mode === '流水') {
      body = html`<${FlowView} data=${d} highlightId=${props.highlightId} onJump=${props.onJump}
        onFlowAction=${props.onFlowAction} />`;
    } else if (d.render_mode === '拦截') {
      body = html`<${InterceptView} data=${d} onAction=${props.onAction} />`;
    } else if (d.render_mode === '遍历') {
      body = html`<${TraverseView} data=${d} entity=${props.entity} onWalk=${props.onWalk} />`;
    } else {
      body = html`<${TerminalTable} data=${d} onAction=${props.onAction} />`;
    }
    return html`<div>${head}${body}</div>`;
  }

  /* ================= 发起事件表单(模态) ================= */
  const SYS_KEYS = ['id', 'actor', 'space', 'time', 'corrects', 'evidence', 'idempotency_key'];

  function FieldControl(props) {
    const f = props.field; // {name, datatype, unit, range, required, options}
    const val = props.value;
    const set = props.onChange;
    const err = props.error;
    // options:服务端按行上下文试探出的合法值(enum 键)。非空数组 → 只含合法值的
    // select;空数组且必填 → 提示"当前状态无合法值";键不在 options 里 → 不干预。
    const hasOptions = Array.isArray(f.options);
    const noLegal = hasOptions && f.options.length === 0 && !!f.required;
    let ctrl = null;
    if (f.datatype === 'enum' || hasOptions) {
      let opts = hasOptions ? f.options : f.range;
      if (typeof opts === 'string') opts = opts.split(/[|,]/).map(function (s) { return s.trim(); }).filter(Boolean);
      if (!Array.isArray(opts)) opts = [];
      ctrl = html`<select value=${val === undefined ? '' : val}
        onChange=${function (e) { set(e.target.value); }}>
        <option value="">(未选择)</option>
        ${opts.map(function (o) { return html`<option key=${o} value=${o}>${o}</option>`; })}
      </select>`;
    } else if (f.datatype === 'boolean') {
      ctrl = html`<input type="checkbox" checked=${!!val}
        onChange=${function (e) { set(e.target.checked); }} />`;
    } else if (f.datatype === 'integer' || f.datatype === 'number') {
      ctrl = html`<span>
        <input type="number" step=${f.datatype === 'integer' ? '1' : 'any'} value=${val === undefined ? '' : val}
          onChange=${function (e) { set(e.target.value); }} />
        ${f.unit && html`<span class="unit">${f.unit}</span>`}
      </span>`;
    } else if (f.datatype === 'ref') {
      ctrl = html`<span>
        <input type="text" list="ontology-ids" value=${val === undefined ? '' : val}
          placeholder="本体 id(可手输)" onChange=${function (e) { set(e.target.value); }} />
      </span>`;
    } else if (f.datatype === 'list' || f.datatype === 'object') {
      ctrl = html`<textarea rows="3" placeholder=${f.datatype === 'list' ? '["..."]' : '{"k":"v"}'}
        value=${val === undefined ? '' : val}
        onChange=${function (e) { set(e.target.value); }}></textarea>`;
    } else {
      ctrl = html`<input type="text" value=${val === undefined ? '' : val}
        onChange=${function (e) { set(e.target.value); }} />`;
    }
    return html`<div class="ctrl">${ctrl}
      ${noLegal && html`<div class="field-err">当前状态无合法值</div>`}
      ${err && html`<div class="field-err">${err}</div>`}</div>`;
  }

  function EventFormModal(props) {
    const types = props.eventTypes || [];
    const attrMap = props.attrMap || {};
    const prefill = props.prefill || {};
    /* 预填统一:presets(按钮/动作的语义自带写入;按钮携带的 prefill.presets
       优先,回退类型注册表的 presets)+ id + corrects(行内修正)+ space
       (视图 queries.spacetime 非空时预填该锚点)。字段控件照常可改(用户可改,
       写侧规则兜底) */
    function presetsOf(name) {
      if (name && name === prefill.type && prefill.presets && typeof prefill.presets === 'object') {
        return prefill.presets;
      }
      const t = types.find(function (x) { return x.type === name; });
      return (t && t.presets) || {};
    }
    function applyPresets(obj, name) {
      const ps = presetsOf(name);
      Object.keys(ps).forEach(function (k) {
        const v = ps[k];
        obj[k] = (v !== null && typeof v === 'object') ? JSON.stringify(v) : v;
      });
    }
    const [typeName, setTypeName] = useState(prefill.type || (types[0] && types[0].type) || '');
    const [vals, setVals] = useState(function () {
      const init = {
        id: prefill.id !== undefined ? String(prefill.id) : '',
        actor: 'ui-user',
        space: prefill.space || props.viewSpace || '',
        space_custom: '',
        time: new Date().toISOString(),
        corrects: (prefill.corrects !== undefined && prefill.corrects !== null) ? String(prefill.corrects) : '',
        evidence: '',
        idempotency_key: ''
      };
      applyPresets(init, prefill.type || (types[0] && types[0].type) || '');
      return init;
    });
    const [errs, setErrs] = useState({});
    const [receipt, setReceipt] = useState(null);
    const [submitting, setSubmitting] = useState(false);

    const typeDef = useMemo(function () {
      return types.find(function (t) { return t.type === typeName; }) || null;
    }, [types, typeName]);

    const attrKeys = useMemo(function () {
      if (!typeDef) return [];
      const req = typeDef.required_keys || [];
      const opt = typeDef.optional_keys || [];
      const presets = typeDef.presets || {};
      // 按钮携带的 options 只对打开时的类型有效(切换类型后无行上下文,不套用)
      const preOpts = (prefill.type === typeName && prefill.options && typeof prefill.options === 'object')
        ? prefill.options : {};
      const all = req.concat(opt).filter(function (k) { return SYS_KEYS.indexOf(k) < 0; });
      return all.filter(function (k, i) { return all.indexOf(k) === i; }).map(function (k) {
        const a = attrMap[k] || {};
        return {
          name: k,
          datatype: a.datatype || 'string',
          unit: a.unit,
          range: a.range,
          semantic: a.semantic,
          required: req.indexOf(k) >= 0,
          preset: Object.prototype.hasOwnProperty.call(presets, k),
          options: Object.prototype.hasOwnProperty.call(preOpts, k) ? preOpts[k] : undefined
        };
      });
    }, [typeDef, attrMap]);

    function setVal(k, v) {
      setVals(function (prev) { const n = Object.assign({}, prev); n[k] = v; return n; });
      setErrs(function (prev) { const n = Object.assign({}, prev); delete n[k]; return n; });
    }

    function buildPayload() {
      const e = {};
      const p = { type: typeName };
      if (!typeName) { e.type = '请选择事件类型'; }
      const idRaw = (vals.id || '').trim();
      if (!idRaw) {
        e.id = 'id 必填';
      } else if (typeDef && typeDef.multi_target) {
        p.id = idRaw.split(',').map(function (s) { return s.trim(); }).filter(Boolean);
        if (p.id.length === 0) e.id = 'id 必填';
      } else {
        p.id = idRaw;
      }
      if (vals.actor) p.actor = vals.actor;
      const sp = vals.space === '__custom__' ? (vals.space_custom || '').trim() : vals.space;
      if (sp) p.space = sp;
      if (vals.time) p.time = vals.time;
      if (vals.corrects !== '' && vals.corrects !== undefined) {
        const n = Number(vals.corrects);
        if (isNaN(n)) e.corrects = '必须为数字'; else p.corrects = n;
      }
      if (vals.evidence) p.evidence = vals.evidence;
      if (vals.idempotency_key) p.idempotency_key = vals.idempotency_key;

      attrKeys.forEach(function (f) {
        const v = vals[f.name];
        const empty = v === undefined || v === '' || v === null;
        if (f.required && f.datatype !== 'boolean' && empty) { e[f.name] = '必填'; return; }
        if (empty && f.datatype !== 'boolean') return;
        if (f.datatype === 'boolean') { p[f.name] = !!v; return; }
        if (f.datatype === 'integer') {
          const n = Number(v);
          if (!Number.isInteger(n)) { e[f.name] = '必须为整数'; return; }
          p[f.name] = n; return;
        }
        if (f.datatype === 'number') {
          const n = Number(v);
          if (isNaN(n)) { e[f.name] = '必须为数字'; return; }
          p[f.name] = n; return;
        }
        if (f.datatype === 'list' || f.datatype === 'object') {
          try {
            const parsed = JSON.parse(v);
            if (f.datatype === 'list' && !Array.isArray(parsed)) { e[f.name] = '必须为 JSON 数组'; return; }
            if (f.datatype === 'object' && (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed))) { e[f.name] = '必须为 JSON 对象'; return; }
            p[f.name] = parsed;
          } catch (ex) { e[f.name] = 'JSON 解析失败: ' + ex.message; }
          return;
        }
        p[f.name] = v;
      });
      return { payload: p, errors: e };
    }

    async function submit() {
      const r = buildPayload();
      setErrs(r.errors);
      if (Object.keys(r.errors).length > 0) return;
      setSubmitting(true);
      setReceipt(null);
      try {
        const resp = await api('/events', { method: 'POST', body: r.payload }, props.token);
        setReceipt(resp);
        if (resp && (resp.status === 'settled' || resp.status === 'accepted')) {
          props.toast('ok', resp.status === 'settled' ? ('已落账 #' + resp.event_id) : ('已受理 队列#' + resp.queue_seq));
          if (resp.status === 'settled') props.onChanged();
        } else if (resp && resp.status === 'rejected') {
          props.toast('err', '事件被拒绝 L' + resp.layer);
        }
      } catch (ex) {
        setReceipt({ status: 'rejected', layer: '?', violations: [ex.message] });
        props.toast('err', '提交失败: ' + ex.message);
      }
      setSubmitting(false);
    }

    async function drain() {
      try {
        const resp = await api('/drain', { method: 'POST' }, props.token);
        props.toast('ok', '落账完成 drained=' + (resp && resp.drained));
        props.onChanged();
      } catch (ex) {
        props.toast('err', '落账失败: ' + ex.message);
      }
    }

    function sysRow(key, label, ctrl, required) {
      return html`<div class="form-row" key=${key}>
        <label>${label || key}${required && html`<span class="req">*</span>`}</label>
        <div class="ctrl">${ctrl}${errs[key] && html`<div class="field-err">${errs[key]}</div>`}</div>
      </div>`;
    }

    const anchors = props.anchors || [];
    return html`<div class="modal-mask" onClick=${function (e) { if (e.target === e.currentTarget) props.onClose(); }}>
      <div class="modal">
        <div class="modal-head">
          <span>发起事件</span>
          <button class="btn" onClick=${props.onClose}>✕</button>
        </div>
        <div class="modal-body">
          <div class="form-row">
            <label>事件类型<span class="req">*</span>${typeDef && typeDef.min_trust !== undefined && html`<span class="dt">min_trust=${typeDef.min_trust}</span>`}</label>
            <div class="ctrl">
              <select value=${typeName} onChange=${function (e) {
                const name = e.target.value;
                setTypeName(name);
                setVals(function (prev) {
                  const n = Object.assign({}, prev);
                  applyPresets(n, name);
                  return n;
                });
              }}>
                ${types.map(function (t) { return html`<option key=${t.type} value=${t.type}>${t.type}</option>`; })}
              </select>
              ${errs.type && html`<div class="field-err">${errs.type}</div>`}
            </div>
          </div>

          <div class="form-sec">系统键</div>
          ${sysRow('id', 'id' + (typeDef && typeDef.multi_target ? '(多目标,逗号分隔)' : ''),
            html`<input type="text" value=${vals.id} onChange=${function (e) { setVal('id', e.target.value); }} />`, true)}
          ${sysRow('actor', 'actor',
            html`<input type="text" value=${vals.actor} onChange=${function (e) { setVal('actor', e.target.value); }} />`)}
          ${sysRow('space', 'space',
            html`<span>
              <select value=${vals.space} onChange=${function (e) { setVal('space', e.target.value); }}>
                <option value="">(不填)</option>
                ${anchors.map(function (a) { return html`<option key=${a.path} value=${a.path}>${a.path}${a.name ? ' (' + a.name + ')' : ''}</option>`; })}
                <option value="__custom__">模糊文本(自由输入)…</option>
              </select>
              ${vals.space === '__custom__' && html`<input type="text" style=${{marginTop:"4px"}} placeholder="原始位置文本"
                value=${vals.space_custom} onChange=${function (e) { setVal('space_custom', e.target.value); }} />`}
            </span>`)}
          ${sysRow('time', 'time(ISO)',
            html`<input type="text" value=${vals.time} onChange=${function (e) { setVal('time', e.target.value); }} />`)}
          ${sysRow('corrects', 'corrects(纠事件号)',
            html`<input type="number" step="1" value=${vals.corrects} onChange=${function (e) { setVal('corrects', e.target.value); }} />`)}
          ${sysRow('evidence', 'evidence',
            html`<input type="text" value=${vals.evidence} onChange=${function (e) { setVal('evidence', e.target.value); }} />`)}
          ${sysRow('idempotency_key', 'idempotency_key',
            html`<input type="text" value=${vals.idempotency_key} onChange=${function (e) { setVal('idempotency_key', e.target.value); }} />`)}

          <div class="form-sec">属性键</div>
          ${attrKeys.length === 0 && html`<div class="muted">该类型无属性键</div>`}
          ${attrKeys.map(function (f) {
            return html`<div class="form-row" key=${f.name}>
              <label title=${f.semantic || ''}>${f.name}${f.required && html`<span class="req">*</span>`}
                ${f.preset && html`<span class="dt" title="类型预置值:按钮语义自带,可改;写侧规则兜底">●</span>`}
                <span class="dt">${f.datatype}${f.unit ? ' · ' + f.unit : ''}</span>
              </label>
              <${FieldControl} field=${f} value=${vals[f.name]} error=${errs[f.name]}
                onChange=${function (v) { setVal(f.name, v); }} />
            </div>`;
          })}

          ${receipt && html`
            ${receipt.status === 'settled' && html`<div class="receipt settled">✔ 已落账 settled,event_id = ${receipt.event_id}</div>`}
            ${receipt.status === 'accepted' && html`<div class="receipt accepted">
              ◆ 已受理 accepted,queue_seq = ${receipt.queue_seq}(异步结算队列)
              <button class="btn" style=${{marginLeft:"8px"}} onClick=${drain}>落账(POST /drain)</button>
            </div>`}
            ${receipt.status === 'rejected' && html`<div class="receipt rejected">
              ✖ 被拒绝 rejected,layer = ${receipt.layer}(${LAYER_NAMES[receipt.layer] || '未知'})
              <pre>${JSON.stringify(receipt.violations, null, 2)}</pre>
            </div>`}
          `}
        </div>
        <div class="modal-foot">
          <button class="btn" onClick=${props.onClose}>关闭</button>
          <button class="btn primary" disabled=${submitting} onClick=${submit}>${submitting ? '提交中…' : '提交事件'}</button>
        </div>
      </div>
    </div>`;
  }

  /* ================= 事件流侧栏 ================= */
  function EventSidebar(props) {
    const [expanded, setExpanded] = useState(null);
    if (props.collapsed) {
      return html`<div class="sidebar-right collapsed" onClick=${props.onToggle} title="展开事件流">
        <span class="collapse-hint">◀ 事件流</span>
      </div>`;
    }
    const evts = props.events || [];
    return html`<div class="sidebar-right">
      <div class="side-title">
        <span>最近事件(${evts.length})</span>
        <button class="btn" onClick=${props.onToggle}>收起 ▶</button>
      </div>
      ${evts.map(function (ev) {
        const open = expanded === ev.event_id;
        return html`<div key=${ev.event_id} class=${'evt-item' + (ev.corrects ? ' correction' : '')}
          onClick=${function () { setExpanded(open ? null : ev.event_id); }}>
          <div class="evt-head">
            <span class="evt-type">#${ev.event_id} ${ev.type}</span>
            <span class="muted mono">${ev.settle_seq !== undefined ? 'seq ' + ev.settle_seq : ''}</span>
          </div>
          <div class="evt-meta">${ev.actor || ''} · ${spaceText(ev.space)} · ${ev.occur_time || ''}</div>
          ${open && html`<pre>${JSON.stringify(ev, null, 2)}</pre>`}
        </div>`;
      })}
      ${evts.length === 0 && html`<div class="muted">(暂无事件)</div>`}
    </div>`;
  }

  /* ================= 顶部工具条 ================= */
  function Toolbar(props) {
    const variants = props.currentViewMeta && props.currentViewMeta.variants
      ? Object.keys(props.currentViewMeta.variants) : [];
    return html`<div class="toolbar">
      <span class="brand">车间世界模型 MES 验证台</span>
      <label>观察者
        <select value=${props.observer} onChange=${function (e) { props.setObserver(e.target.value); }}>
          <option value="">默认</option>
          ${variants.map(function (v) { return html`<option key=${v} value=${v}>${v}</option>`; })}
        </select>
      </label>
      <label>entity
        <input type="text" list="ontology-ids" value=${props.entity} placeholder="实体 id"
          onChange=${function (e) { props.setEntity(e.target.value); }} />
      </label>
      <datalist id="ontology-ids">
        ${(props.ontologies || []).map(function (o) { return html`<option key=${o.id} value=${o.id} />`; })}
      </datalist>
      <label>AS OF t
        <input type="number" step="1" style=${{width:"90px"}} value=${props.asOf} placeholder="空=当前"
          onChange=${function (e) { props.setAsOf(e.target.value); }} />
      </label>
      <label>凭证
        <input type="text" style=${{width:"120px"}} value=${props.token} title="X-MSE-Token"
          onChange=${function (e) { props.setToken(e.target.value); }} />
      </label>
      <button class="btn primary" onClick=${props.onRefresh}>刷新</button>
      <span class=${'toggle' + (props.auto ? ' on' : '')} onClick=${function () { props.setAuto(!props.auto); }}>
        ${props.auto ? '◉' : '○'} 自动刷新(2s)
      </span>
      <button class="btn" onClick=${function () { props.onEmit(null); }}>+ 发起事件</button>
      <button class="btn" title="POST /drain" onClick=${props.onDrain}>落账</button>
    </div>`;
  }

  /* ================= App ================= */
  function App() {
    const [views, setViews] = useState(null);
    const [eventTypes, setEventTypes] = useState([]);
    const [attrMap, setAttrMap] = useState({});
    const [anchors, setAnchors] = useState([]);
    const [ontologies, setOntologies] = useState([]);
    // 深链接:加载时解析 hash 选中视图与参数(encodeURIComponent 编码)
    const [selectedView, setSelectedView] = useState(function () {
      return parseHash().view || null;
    });
    const [observer, setObserver] = useState(function () {
      return parseHash().observer || '';
    });
    const [entity, setEntity] = useState(function () {
      return parseHash().entity || '';
    });
    const [asOf, setAsOf] = useState(function () {
      return parseHash().t || '';
    });
    const [token, setToken] = useState(function () {
      try { return localStorage.getItem('mse-token') || 'ui-token'; } catch (e) { return 'ui-token'; }
    });
    const [viewData, setViewData] = useState(null);
    const [viewErr, setViewErr] = useState(null);
    const [loading, setLoading] = useState(false);
    const [auto, setAuto] = useState(false);
    const [events, setEvents] = useState([]);
    const [collapsed, setCollapsed] = useState(false);
    const [modal, setModal] = useState(null); // {type, id} | null
    const [toasts, setToasts] = useState([]);
    const [highlightId, setHighlightId] = useState(null);

    const toast = useCallback(function (kind, text) {
      const id = ++toastId;
      setToasts(function (prev) { return prev.concat([{ id: id, kind: kind, text: text }]); });
      setTimeout(function () {
        setToasts(function (prev) { return prev.filter(function (t) { return t.id !== id; }); });
      }, 4500);
    }, []);

    /* 初始元数据 */
    useEffect(function () {
      api('/meta/views').then(function (v) {
        setViews(v || []);
        if (v && v.length && !selectedView) setSelectedView(v[0].view_id);
      }).catch(function (e) { toast('err', '加载视图失败: ' + e.message); });
      api('/meta/event-types').then(function (v) { setEventTypes(v || []); })
        .catch(function (e) { toast('err', '加载事件类型失败: ' + e.message); });
      api('/meta/attributes').then(function (v) {
        const m = {};
        (v || []).forEach(function (a) { m[a.key] = a; });
        setAttrMap(m);
      }).catch(function (e) { toast('err', '加载属性字典失败: ' + e.message); });
      api('/meta/anchors').then(function (v) { setAnchors(v || []); })
        .catch(function (e) { toast('err', '加载锚点失败: ' + e.message); });
      api('/meta/ontologies').then(function (v) { setOntologies(v || []); })
        .catch(function (e) { toast('err', '加载本体失败: ' + e.message); });
    }, []);

    const loadEvents = useCallback(function () {
      api('/meta/events?limit=30').then(function (v) { setEvents(v || []); })
        .catch(function (e) { toast('err', '加载事件流失败: ' + e.message); });
    }, [toast]);

    const loadView = useCallback(function () {
      if (!selectedView) { setViewData(null); return; }
      const q = [];
      if (observer) q.push('observer=' + encodeURIComponent(observer));
      if (entity) q.push('entity=' + encodeURIComponent(entity));
      if (asOf !== '') q.push('t=' + encodeURIComponent(asOf));
      setLoading(true);
      setViewErr(null);
      api('/views/' + encodeURIComponent(selectedView) + (q.length ? '?' + q.join('&') : ''))
        .then(function (d) { setViewData(d); setLoading(false); })
        .catch(function (e) { setViewErr(e.message); setLoading(false); });
    }, [selectedView, observer, entity, asOf]);

    useEffect(function () { loadView(); }, [loadView]);
    useEffect(function () { loadEvents(); }, [loadEvents]);

    /* 自动刷新 2s */
    useEffect(function () {
      if (!auto) return;
      const timer = setInterval(function () { loadView(); loadEvents(); }, 2000);
      return function () { clearInterval(timer); };
    }, [auto, loadView, loadEvents]);

    function refreshAll() { loadView(); loadEvents(); }

    async function drain() {
      try {
        const resp = await api('/drain', { method: 'POST' }, token);
        toast('ok', '落账完成 drained=' + (resp && resp.drained));
        refreshAll();
      } catch (e) { toast('err', '落账失败: ' + e.message); }
    }

    // 行内/行级按钮:带上按钮的 presets/options(该行的合法选项上下文)开表单
    function onAction(type, id, btn) {
      setModal({ type: type, id: id,
                 presets: btn && btn.presets, options: btn && btn.options });
    }
    // 流水行内动作:id 与 corrects(修正类型)由服务端随按钮下发
    function onFlowAction(btn) {
      setModal({ type: btn.type, id: btn.id,
                 presets: btn.presets, options: btn.options,
                 corrects: btn.corrects });
    }

    function onJump(eventId) {
      setHighlightId(eventId);
      const el = document.getElementById('flow-evt-' + eventId);
      if (el && el.scrollIntoView) el.scrollIntoView({ block: 'center', behavior: 'smooth' });
      setTimeout(function () { setHighlightId(null); }, 1800);
    }

    function onWalk(id) { setEntity(id); }

    const currentViewMeta = useMemo(function () {
      return (views || []).find(function (v) { return v.view_id === selectedView; }) || null;
    }, [views, selectedView]);

    /* 切换视图时重置 observer(角色属于视图 variants);深链接进入/写回不算切换 */
    const lastViewRef = useRef(selectedView);
    useEffect(function () {
      if (lastViewRef.current === selectedView) return;
      lastViewRef.current = selectedView;
      setObserver('');
    }, [selectedView]);

    /* 深链接写回:视图/参数变化时同步 hash(replaceState,不刷历史、不触发
       hashchange);hashchange(手改/前进后退)→ 解析回状态 */
    useEffect(function () {
      const h = buildHash(selectedView, entity, observer, asOf);
      if (window.location.hash !== h) {
        history.replaceState(null, '', window.location.pathname + window.location.search + h);
      }
    }, [selectedView, entity, observer, asOf]);
    useEffect(function () {
      function onHash() {
        const p = parseHash();
        if (p.view) { lastViewRef.current = p.view; setSelectedView(p.view); }
        if (p.entity !== undefined) setEntity(p.entity);
        if (p.observer !== undefined) setObserver(p.observer);
        if (p.t !== undefined) setAsOf(p.t);
      }
      window.addEventListener('hashchange', onHash);
      return function () { window.removeEventListener('hashchange', onHash); };
    }, []);

    /* 持久化 token */
    useEffect(function () {
      try { localStorage.setItem('mse-token', token); } catch (e) { /* ignore */ }
    }, [token]);

    return html`<${React.Fragment}>
      <${Toolbar}
        currentViewMeta=${currentViewMeta}
        observer=${observer} setObserver=${setObserver}
        entity=${entity} setEntity=${setEntity}
        asOf=${asOf} setAsOf=${setAsOf}
        token=${token} setToken=${setToken}
        ontologies=${ontologies}
        onRefresh=${refreshAll}
        auto=${auto} setAuto=${setAuto}
        onEmit=${function () { setModal({}); }}
        onDrain=${drain} />
      <div class="main">
        <div class="sidebar-left">
          <${ViewList} views=${views} selected=${selectedView} onSelect=${setSelectedView} />
        </div>
        <div class="content">
          <${ViewContent}
            viewId=${selectedView}
            data=${viewData} error=${viewErr} loading=${loading}
            entity=${entity}
            highlightId=${highlightId} onJump=${onJump}
            onAction=${onAction} onFlowAction=${onFlowAction} onWalk=${onWalk} />
        </div>
        <${EventSidebar} events=${events} collapsed=${collapsed}
          onToggle=${function () { setCollapsed(!collapsed); }} />
      </div>
      ${modal && html`<${EventFormModal}
        prefill=${modal}
        viewSpace=${(currentViewMeta && currentViewMeta.queries && currentViewMeta.queries.spacetime) || ''}
        eventTypes=${eventTypes} attrMap=${attrMap}
        anchors=${anchors} token=${token}
        toast=${toast}
        onChanged=${refreshAll}
        onClose=${function () { setModal(null); }} />`}
      <${Toasts} toasts=${toasts} />
    </>`;
  }

  ReactDOM.createRoot(document.getElementById('root')).render(html`<${App} />`);
})();
