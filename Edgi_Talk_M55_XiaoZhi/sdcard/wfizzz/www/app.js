/* WFIZZZ 配网页逻辑
 *
 * 与固件的约定（见 web_cfg.c）：
 *   GET  /cgi-bin/status  -> {state, ip, persist, dirty, ssid, rssi}
 *   GET  /cgi-bin/get     -> {字段名: 值, ...}   敏感字段已是 "****后4位"
 *   POST /cgi-bin/set     -> 表单 body，字段名即 key；空值 = 不改
 *   GET  /cgi-bin/scan    -> [{ssid, rssi, channel, sec}, ...]
 *   POST /cgi-bin/wifi    -> 表单 ssid= & pass=
 *   POST /cgi-bin/reboot
 *
 * 两条硬约束：
 *   1) 请求必须串行。板子主堆只剩约 40KB，每个连接占 4.1KB，
 *      并发 6 个就把堆吃光。所以这里用一个 Promise 队列。
 *   2) 敏感字段回显是掩码。用户不改就保持空，提交时空值会被固件忽略，
 *      所以绝不能把掩码值再提交回去——那会把 "****abcd" 存成真密码。
 *      下面 collect() 里对 secret 字段有专门处理。
 */

(function () {
  'use strict';

  /* ---------- 字段表：与 app_cfg.h 的 key 一一对应 ---------- */
  var FIELDS = [
    'wifi_ssid', 'wifi_pass',
    /* xiaozhi_port / xiaozhi_ota_port / xiaozhi_ssl 是 2026-10-26 新增：
     * 支持连自建服务端（局域网 ws://ip:8000，OTA 在 8003）。
     * 三者都是普通字符串字段，固件侧当数字/开关解析。 */
    'xiaozhi_host', 'xiaozhi_port', 'xiaozhi_ota_port', 'xiaozhi_ssl',
    'xiaozhi_path', 'xiaozhi_token',
    'memo_host', 'memo_token',
    'weather_host', 'weather_path', 'weather_city', 'weather_key',
    'llm_host', 'llm_path', 'llm_key', 'llm_model'
  ];

  /* 需要掩码处理的字段。固件侧 app_cfg_is_secret 是唯一权威，
   * 这里只用于前端判断"不要回填"。 */
  var SECRET = {
    wifi_pass: 1, xiaozhi_token: 1, memo_token: 1,
    weather_key: 1, llm_key: 1
  };

  /* ---------- 串行请求队列 ---------- */
  var chain = Promise.resolve();

  function enqueue(fn) {
    var p = chain.then(fn, fn);
    /* 队列本身不能因为某次失败而断掉 */
    chain = p.catch(function () {});
    return p;
  }

  function request(method, path, body) {
    return enqueue(function () {
      var opt = {
        method: method,
        headers: {},
        cache: 'no-store'
      };
      if (body) {
        opt.headers['Content-Type'] = 'application/x-www-form-urlencoded';
        opt.body = body;
      }
      return fetch(path, opt).then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.text();
      });
    });
  }

  function getJSON(path) {
    return request('GET', path).then(function (t) {
      return JSON.parse(t);
    });
  }

  function post(path, obj) {
    var parts = [];
    for (var k in obj) {
      if (!Object.prototype.hasOwnProperty.call(obj, k)) continue;
      /* 值编码两次。原因：板子上的 webnet 2.0.3 解析参数时，解完码
       * 又拿同一个缓冲区继续扫 '&' 和 '='，于是值里解码出来的这两个
       * 字符会被当成分隔符 —— 天气路径
       * "/data/2.5/weather?q=Beijing&units=metric" 会被截成 "Beijing"。
       * 编两层后 webnet 解掉一层只剩 %XX，不再触发截断；板子侧
       * web_cfg.c 的 param() 再把第二层解掉。
       * 键名不需要双层（键里本来就不允许 & 和 =）。 */
      parts.push(encodeURIComponent(k) + '=' +
                 encodeURIComponent(encodeURIComponent(String(obj[k]))));
    }
    return request('POST', path, parts.join('&')).then(function (t) {
      return JSON.parse(t);
    });
  }

  /* ---------- UI 工具 ---------- */
  var toastTimer = null;

  function toast(msg, kind) {
    var el = document.getElementById('toast');
    if (!el) return;
    el.textContent = msg;
    el.className = 'toast show' + (kind ? ' ' + kind : '');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      el.className = 'toast' + (kind ? ' ' + kind : '');
    }, 3200);
  }

  function $(id) { return document.getElementById(id); }

  function val(id) {
    var el = $(id);
    return el ? el.value.trim() : '';
  }

  function setVal(id, v) {
    var el = $(id);
    if (el) el.value = (v === undefined || v === null) ? '' : v;
  }

  /* ---------- 状态栏 ---------- */
  var STATE_TEXT = {
    0: '未配网',
    1: '正在连接 WiFi…',
    2: '已连接',
    3: '配置热点已启动'
  };

  function setText(id, v) {
    var el = $(id);
    if (el) el.textContent = (v === undefined || v === null || v === '') ? '—' : v;
  }

  function refreshStatus() {
    return getJSON('/cgi-bin/status').then(function (s) {
      var dot = $('dot');
      var txt = $('statustext');

      var label = STATE_TEXT[s.state] || '未知状态';

      /* 图签只有一格「连接」放状态词，IP 与信号各自有独立字段格，
       * 所以这里不再拼 ip / rssi，避免同一信息在页头重复两遍。 */
      if (s.state === 2) {
        dot.className = 'dot ok';
      } else if (s.state === 3) {
        dot.className = 'dot warn';
      } else if (s.state === 1) {
        dot.className = 'dot warn';
      } else {
        dot.className = 'dot err';
      }

      if (txt) txt.textContent = label;

      /* 图签字段格：地址 / 信号 / 存储 */
      setText('ro-ip', s.ip && s.ip !== '-' ? s.ip : '—');
      setText('ro-rssi', s.rssi ? s.rssi + ' dBm' : '—');
      setText('ro-store', s.persist ? (s.dirty ? '待写盘' : 'SD 卡') : '仅内存');
      setText('ro-wifi', s.ssid ? s.ssid : '未配置');

      var hint = $('persist-hint');
      if (hint) {
        if (s.persist) {
          hint.innerHTML = '配置保存在 SD 卡 <code>/sdcard/wfizzz/cfg.json</code>。' +
                           (s.dirty ? '<br><span class="warn">有改动尚未写入，请检查存储卡。</span>' : '');
        } else {
          hint.innerHTML = '<span class="warn">未检测到可写的 SD 卡。</span>' +
                           '当前修改只在本次运行期间有效，重启后丢失。' +
                           '插卡后可在串口执行 <code>cfg_reload</code> 重试。';
        }
      }
    }).catch(function () {
      var txt = $('statustext');
      if (txt) txt.textContent = '设备无响应';
      var dot = $('dot');
      if (dot) dot.className = 'dot err';
      setText('ro-ip', '—');
      setText('ro-rssi', '—');
      setText('ro-store', '—');
      setText('ro-wifi', '—');
    });
  }

  /* ---------- 载入配置 ---------- */

  /* 每张服务卡标出是否已配置完整。判断依据是"真正连得上所需的字段都非空"，
   * 所以令牌也算关键字段——地址填了但没令牌，服务同样用不了。
   * 掩码值算已配置（固件给 "****abcd" 说明该字段存过东西）。
   * 空值在这里是正常状态，所以不用警告色，只用中性文字。 */
  var READY_RULES = [
    { id: 'st-wifi',    keys: ['wifi_ssid', 'wifi_pass'] },
    { id: 'st-xiaozhi', keys: ['xiaozhi_host', 'xiaozhi_port', 'xiaozhi_ota_port',
                               'xiaozhi_ssl', 'xiaozhi_path', 'xiaozhi_token'] },
    { id: 'st-memo',    keys: ['memo_host', 'memo_token'] },
    { id: 'st-weather', keys: ['weather_host', 'weather_path', 'weather_city', 'weather_key'] },
    { id: 'st-llm',     keys: ['llm_host', 'llm_path', 'llm_key', 'llm_model'] }
  ];

  function markReady(cfg) {
    READY_RULES.forEach(function (r) {
      var el = $(r.id);
      if (!el) return;
      var ready = r.keys.every(function (k) {
        return cfg[k] && String(cfg[k]).length > 0;
      });
      el.textContent = ready ? '已配置' : '待填写';
      el.className = 'secstate' + (ready ? ' ok' : '');
    });
  }

  function loadConfig() {
    return getJSON('/cgi-bin/get').then(function (cfg) {
      FIELDS.forEach(function (k) {
        var el = $(k);
        if (!el) return;

        /* 敏感字段：固件给的是掩码，绝不能填进输入框——
         * 否则用户一提交就把 "****abcd" 存成真密码。
         * 改为用 placeholder 提示"已设置"。 */
        if (SECRET[k]) {
          var has = cfg[k] && cfg[k].length > 0;
          el.value = '';
          el.placeholder = has ? '已设置（' + cfg[k] + '），留空不修改' : '未设置';
        } else {
          el.value = cfg[k] || '';
        }
      });
      markReady(cfg);
    });
  }

  /* ---------- 收集改动 ---------- */
  function collect() {
    var out = {};

    FIELDS.forEach(function (k) {
      var el = $(k);
      if (!el) return;

      var v = el.value.trim();

      if (SECRET[k]) {
        /* 空 = 不改。有值才提交。 */
        if (v.length > 0) out[k] = v;
      } else {
        /* 非敏感字段全量提交：用户可能清空某项（比如撤掉模型名） */
        out[k] = v;
      }
    });

    return out;
  }

  /* ---------- 扫描 ---------- */
  function renderScan(list) {
    var box = $('scanlist');
    if (!box) return;

    box.innerHTML = '';
    box.classList.remove('hidden');

    if (!list || list.length === 0) {
      box.innerHTML = '<div class="scanempty">没有扫到网络。请靠近路由器后重试。</div>';
      return;
    }

    list.forEach(function (ap) {
      var row = document.createElement('div');
      row.className = 'scanitem';

      var name = document.createElement('span');
      name.className = 'name';
      name.textContent = ap.ssid;

      var meta = document.createElement('span');
      meta.className = 'meta';
      meta.textContent = ap.rssi + 'dBm';

      var lock = document.createElement('span');
      lock.className = 'lock';
      lock.textContent = ap.sec ? '🔒' : '';

      row.appendChild(name);
      if (ap.sec) row.appendChild(lock);
      row.appendChild(meta);

      row.addEventListener('click', function () {
        setVal('wifi_ssid', ap.ssid);
        $('wifi_pass').focus();
        box.classList.add('hidden');
      });

      box.appendChild(row);
    });
  }

  function doScan() {
    var btn = $('btn-scan');
    if (btn) { btn.disabled = true; btn.textContent = '扫描中'; }

    return getJSON('/cgi-bin/scan').then(function (list) {
      renderScan(list);
    }).catch(function (e) {
      toast('扫描失败：' + e.message, 'err');
    }).then(function () {
      if (btn) { btn.disabled = false; btn.textContent = '扫描'; }
    });
  }

  /* ---------- 动作 ---------- */
  function saveWifi() {
    var ssid = val('wifi_ssid');
    var pass = val('wifi_pass');

    if (!ssid) {
      toast('请先选择或输入 WiFi 名称', 'err');
      return Promise.resolve();
    }

    var btn = $('btn-wifi');
    if (btn) { btn.disabled = true; btn.textContent = '连接中…'; }

    /* 凭据必须逐字段单独保存。板子的 webnet 2.0.3 解析多参数时，
     * 后面的参数会被截断（详见 saveAll 里的说明），而密码恰好在
     * 第二位——一次发 ssid+pass 会把含特殊字符的密码存坏。
     * 存完再调 /cgi-bin/wifi 触发连接（那个 CGI 现在只读内存里的
     * 凭据，不再接受参数）。 */
    return post('/cgi-bin/set', { wifi_ssid: ssid }).then(function () {
      setVal('wifi_pass', '');
      if (!pass) return null;
      return post('/cgi-bin/set', { wifi_pass: pass });
    }).then(function () {
      return post('/cgi-bin/wifi', { go: '1' });
    }).then(function (r) {
      if (r.error) {
        toast('失败：' + r.error, 'err');
        return;
      }
      if (!r.persist) {
        toast('已应用，但未写入 SD 卡（重启会丢失）', 'err');
      } else {
        toast('已保存，正在连接…', 'ok');
      }
      /* 连接要几秒，隔一会儿再刷状态 */
      setTimeout(refreshStatus, 2500);
      setTimeout(refreshStatus, 8000);
    }).catch(function (e) {
      toast('请求失败：' + e.message, 'err');
    }).then(function () {
      if (btn) { btn.disabled = false; btn.textContent = '保存并连接'; }
    });
  }

  function saveAll() {
    var data = collect();
    var btn = $('btn-save');
    var keys = Object.keys(data);

    if (btn) { btn.disabled = true; btn.textContent = '保存中…'; }

    /* 逐个字段单独发一次 POST，不要合并成一个多参数请求。
     *
     * 原因：板子上的 webnet 2.0.3 解析参数时对 value 就地 urldecode
     * （会缩短字符串），但已解析项与后续项的指针不会重新对齐。
     * 实测只要 weather_path 不是**第一个**参数，它的值就必然被截断
     * （"/data/2.5/weather?q=X&units=metric" -> "X"），与前面参数
     * 长短无关。单参数请求则稳定正确。
     *
     * 代价是 N 次往返，但配置页不是高频操作，换来正确性值得。
     * 顺带好处：某个字段失败不会连累其它字段。 */
    var okTotal = 0, dirtyRisk = false, failed = [];

    var seq = Promise.resolve();
    keys.forEach(function (k) {
      seq = seq.then(function () {
        var one = {};
        one[k] = data[k];
        return post('/cgi-bin/set', one).then(function (r) {
          if (r && r.ok) okTotal += r.ok;
          if (r && !r.persist) dirtyRisk = true;
        }).catch(function () {
          failed.push(k);
        });
      });
    });

    return seq.then(function () {
      if (failed.length) {
        toast('部分项保存失败：' + failed.join(', '), 'err');
      } else if (dirtyRisk) {
        toast('已应用，但未写入 SD 卡（重启会丢失）', 'err');
      } else if (okTotal === 0) {
        toast('没有需要保存的改动', '');
      } else {
        toast('已保存 ' + okTotal + ' 项', 'ok');
      }
      /* 重新拉一遍，把掩码 placeholder 更新掉 */
      return loadConfig();
    }).catch(function (e) {
      toast('请求失败：' + e.message, 'err');
    }).then(function () {
      if (btn) { btn.disabled = false; btn.textContent = '保存全部'; }
    });
  }

  function clearField(key) {
    var body = {};
    body['clear:' + key] = '1';

    return post('/cgi-bin/set', body).then(function (r) {
      if (r.error) {
        toast('失败：' + r.error, 'err');
        return;
      }
      toast('已清除', 'ok');
      return loadConfig();
    }).catch(function (e) {
      toast('请求失败：' + e.message, 'err');
    });
  }

  function reboot() {
    if (!confirm('确定重启设备？未保存的改动会丢失。')) return Promise.resolve();

    /* 先保存，再重启——否则用户会丢改动 */
    return saveAll().then(function () {
      return post('/cgi-bin/reboot', {});
    }).then(function () {
      toast('设备正在重启，约 15 秒后重新连接', 'ok');
      setTimeout(refreshStatus, 18000);
    }).catch(function () {
      /* 重启时连接被切断是正常的，不当失败处理 */
      toast('设备正在重启', 'ok');
      setTimeout(refreshStatus, 18000);
    });
  }

  /* ---------- 绑定 ---------- */
  function bind() {
    var el;

    el = $('btn-scan');   if (el) el.addEventListener('click', doScan);
    el = $('btn-wifi');   if (el) el.addEventListener('click', saveWifi);
    el = $('btn-save');   if (el) el.addEventListener('click', saveAll);
    el = $('btn-reboot'); if (el) el.addEventListener('click', reboot);

    /* 清除按钮 */
    var clears = document.querySelectorAll('[data-clear]');
    Array.prototype.forEach.call(clears, function (b) {
      b.addEventListener('click', function () {
        clearField(b.getAttribute('data-clear'));
      });
    });

    /* 回车提交当前卡片 */
    Array.prototype.forEach.call(document.querySelectorAll('input'), function (inp) {
      inp.addEventListener('keydown', function (e) {
        if (e.key === 'Enter') {
          e.preventDefault();
          inp.blur();
        }
      });
    });
  }

  /* ---------- 启动 ---------- */
  function init() {
    bind();

    refreshStatus();

    /* get 拉配置。失败（比如 SD 页与固件版本不匹配）不影响页面可用性，
     * 只是输入框保持空 —— 用户仍能填新值并保存。 */
    loadConfig().catch(function () {
      toast('读取当前配置失败，页面仍可填写保存', 'err');
    });

    /* 状态轮询：5 秒一次。比 1 秒省电，也比不刷新实用
     * （配网时用户需要看到"已连接"的反馈）。 */
    setInterval(refreshStatus, 5000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
