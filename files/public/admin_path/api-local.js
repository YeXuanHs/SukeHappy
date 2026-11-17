/* 模板后台本地账号改造（前端层）
 *
 * 1) 把发往 apiv1.sukeyun.com 的请求全部转到 /mbadmin/api/router.php（本地库）
 * 2) 首次使用（库里没账号）：标题改成「创建管理员账号」、按钮改成「创建管理员账号」、
 *    并追加一个「请再次输入密码」输入框，两次不一致直接拦住提交
 * 3) 隐藏「忘记密码」「创建新账号」入口
 */
(function () {
  if (window.__mdyApiPatched) return;
  window.__mdyApiPatched = 1;

  var TARGET = 'apiv1.sukeyun.com';
  var LOCAL = '/mbadmin/api/router.php?__p=';
  var API_BASE = '/mbadmin/api/router.php';

  function rewrite(url) {
    if (!url || typeof url !== 'string') return url;
    if (url.indexOf(TARGET) < 0) return url;
    var p = url.replace(/^https?:\/\/[^\/]+/, '');
    return LOCAL + encodeURIComponent(p);
  }

  /* ---------- 1. 劫持 XHR（axios 走这个） ---------- */
  var XO = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (m, u) {
    var nu = rewrite(String(u));
    var args = [].slice.call(arguments);
    args[1] = nu;
    return XO.apply(this, args);
  };

  /* ---------- 2. 劫持 fetch（兜底） ---------- */
  var of = window.fetch;
  if (of) {
    window.fetch = function (u, o) {
      try {
        if (typeof u === 'string') u = rewrite(u);
        else if (u && u.url) u = new Request(rewrite(u.url), u);
      } catch (e) {}
      return of.call(this, u, o);
    };
  }

  var hasAccount = null;

  /* ---------- 3. 按钮文案（Arco 按钮里的文字是直接文本节点） ---------- */
  function forceBtnLabel(label) {
    var btns = document.querySelectorAll('.loginContent .arco-btn.login, .loginContent .el-button.login');
    for (var i = 0; i < btns.length; i++) {
      var w = document.createTreeWalker(btns[i], NodeFilter.SHOW_TEXT, null);
      var n;
      while ((n = w.nextNode())) {
        var v = (n.nodeValue || '').trim();
        if (v && v !== label) {
          n.nodeValue = ' ' + label + ' ';
          return;
        }
      }
    }
  }

  /* ---------- 4. 二次密码框 ---------- */
  function confirmBox() {
    return document.querySelector('.loginContent [data-mdy-confirm]');
  }

  function showConfirmErr(text) {
    var box = confirmBox();
    if (!box) return;
    var msg = box.querySelector('[data-mdy-msg]');
    if (!msg) return;
    if (text) {
      msg.textContent = text;
      msg.style.display = 'block';
    } else {
      msg.textContent = '';
      msg.style.display = 'none';
    }
  }

  function realPasswordInput() {
    var box = document.querySelector('.loginContent [id="password"]');
    if (box) {
      var i = box.querySelector('input');
      if (i) return i;
    }
    return document.querySelector('.loginContent input[type="password"]');
  }

  function ensureConfirmField() {
    if (hasAccount !== false) return;
    if (confirmBox()) return;
    var pw = realPasswordInput();
    if (!pw) return;
    var item = pw.closest ? pw.closest('.arco-form-item') : null;
    var row = item && item.parentElement ? item.parentElement : null;
    if (!row || !row.parentNode) return;

    var clone = row.cloneNode(true);
    clone.setAttribute('data-mdy-confirm', '1');
    var withId = clone.querySelectorAll('[id]');
    for (var i = 0; i < withId.length; i++) withId[i].removeAttribute('id');

    var inp = clone.querySelector('input');
    if (!inp) return;
    inp.value = '';
    inp.setAttribute('placeholder', '请再次输入密码');
    inp.setAttribute('autocomplete', 'new-password');

    /* 眼睛图标：原组件是 Vue 绑定的，克隆体自己接一个简单切换 */
    var eye = clone.querySelector('.arco-input-suffix .arco-icon-hover');
    if (eye) {
      eye.addEventListener('click', function () {
        inp.type = inp.type === 'password' ? 'text' : 'password';
      });
    }

    /* 错误提示 */
    var wrapper = clone.querySelector('.arco-form-item-content-wrapper') || clone;
    var msg = document.createElement('div');
    msg.setAttribute('data-mdy-msg', '1');
    msg.style.cssText = 'display:none;color:#f53f3f;font-size:12px;line-height:1.5;margin:4px 0 0 2px';
    wrapper.appendChild(msg);

    inp.addEventListener('input', function () { showConfirmErr(''); });

    row.parentNode.insertBefore(clone, row.nextSibling);
  }

  function removeConfirmField() {
    var box = confirmBox();
    if (box && box.parentNode) box.parentNode.removeChild(box);
  }

  /* 校验：只在首装（创建）时生效 */
  function validateConfirm() {
    if (hasAccount !== false) return true;
    var box = confirmBox();
    if (!box) return true;
    var c = box.querySelector('input');
    var p = realPasswordInput();
    if (!c || !p) return true;
    if (!c.value) {
      showConfirmErr('请再次输入密码');
      return false;
    }
    if (c.value !== p.value) {
      showConfirmErr('两次输入的密码不一致');
      return false;
    }
    showConfirmErr('');
    return true;
  }

  /* 捕获阶段拦截：校验不过就不让 Vue 的点击/回车处理器跑 */
  document.addEventListener('click', function (e) {
    if (hasAccount !== false) return;
    var t = e.target;
    if (!t || !t.closest) return;
    if (!t.closest('.loginContent .arco-btn.login, .loginContent .el-button.login')) return;
    if (!validateConfirm()) {
      e.stopPropagation();
      e.preventDefault();
    }
  }, true);

  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Enter') return;
    if (hasAccount !== false) return;
    var t = e.target;
    if (!t || !t.closest || !t.closest('.loginContent')) return;
    if (!validateConfirm()) {
      e.stopPropagation();
      e.preventDefault();
    }
  }, true);

  /* ---------- 5. 标题 / 提示条 / 按钮 ---------- */
  function applyFirstTimeUI() {
    if (hasAccount === null) return;
    var root = document.querySelector('.loginContent');
    if (!root) return;

    forceBtnLabel(hasAccount === false ? '创建管理员账号' : '登录');

    /* 浏览器标签标题：创建态不能还写「登录」 */
    var want = hasAccount === false ? '创建' : '登录';
    var suffix = ' - SK模板(财务)';
    if (document.title !== want + suffix) {
      document.title = want + suffix;
    }

    if (hasAccount === false) {
      if (!document.getElementById('mdyFirstTip')) {
        var tip = document.createElement('div');
        tip.id = 'mdyFirstTip';
        tip.style.cssText = 'margin:0 0 12px;padding:10px 12px;border-radius:4px;'
          + 'background:#e8f3ff;color:#0052d9;font-size:13px;line-height:1.6';
        tip.innerText = '首次使用：请设置管理员账号和密码（密码至少 6 位，需输两次确认），创建后此页即为登录页。';
        root.insertBefore(tip, root.firstChild);
      }
      ensureConfirmField();
    } else {
      var t0 = document.getElementById('mdyFirstTip');
      if (t0) t0.parentNode.removeChild(t0);
      removeConfirmField();
    }
  }

  /* ---------- 6. 隐藏忘记密码 / 创建新账号（兜底，节点已从渲染树删除） ---------- */
  function hideExtraLinks() {
    var els = document.querySelectorAll('a, .arco-link, .el-link');
    for (var i = 0; i < els.length; i++) {
      var t = (els[i].innerText || '').replace(/\s/g, '');
      if (t === '忘记密码？' || t === '忘记密码' || t === '创建新账号' || t === '还没有账号?') {
        els[i].style.display = 'none';
      }
    }
  }

  function injectCss() {
    if (document.getElementById('mdyMbCss')) return;
    var st = document.createElement('style');
    st.id = 'mdyMbCss';
    st.textContent = ''
      /* 右侧表单块垂直居中（面板本身是 flex-column 撑满高度，默认贴顶） */
      + '.login-container .form-wrapper.right{justify-content:center!important}'
      + '.loginContent .arco-input-append{display:none!important}'
      + '.loginContent .arco-input-group-addon{display:none!important}'
      + '.loginContent .arco-select{display:none!important}'
      + '.loginContent .arco-input-outer{background:transparent!important}'
      + '.loginContent .arco-tabs-header{display:none!important}'
      + '.loginContent .arco-tabs-nav{display:none!important}'
      + '.loginContent .arco-tabs-tab{display:none!important}'
      + '.loginContent .arco-tabs-content{padding-top:0!important}'
      + '.loginContent .el-input-group__append{display:none!important}'
      + '.loginContent .el-tabs__header{display:none!important}'
      + MDY_MODAL_CSS;
    (document.head || document.documentElement).appendChild(st);
  }

  /* ---------- 7. 用户下拉：重置密码（ElMessageBox 同款外观） ---------- */
  var MDY_MODAL_CSS = ''
    /* 下拉项：EP 只写了 :focus，手插的 li 不在它的 JS 里，补 :hover */
    + 'ul.el-dropdown-menu li[data-mdy-reset]:hover,'
    + 'ul.el-dropdown-menu li[data-mdy-reset]:focus{'
    + 'background-color:var(--el-dropdown-menuItem-hover-fill,#f5f7fa);'
    + 'color:var(--el-dropdown-menuItem-hover-color,var(--el-color-primary))}'
    + 'ul.el-dropdown-menu li[data-mdy-reset]{display:flex;align-items:center}'
    + '#mdyPwdMask{display:flex;align-items:center;justify-content:center;animation:mdyMaskIn .3s}'
    + '#mdyPwdMask>.el-message-box{animation:msgbox-fade-in .3s;width:420px}'
    + '#mdyPwdMask .el-input__prefix{align-items:center;margin-left:1px;margin-right:6px}'
    + '#mdyPwdForm .mdy-fi{margin-bottom:14px}'
    + '#mdyPwdForm .mdy-fi:first-child{margin-top:2px}'
    + '#mdyPwdForm .mdy-fi>label{display:block;margin-bottom:8px;font-size:14px;'
    + 'line-height:20px;color:var(--el-text-color-regular);font-weight:400}'
    + '#mdyPwdErr{min-height:18px;line-height:18px;font-size:12px;'
    + 'color:var(--el-color-danger);margin:-6px 0 2px}'
    + '@keyframes mdyMaskIn{from{opacity:0}to{opacity:1}}'
    + '@keyframes mdyMaskOut{from{opacity:1}to{opacity:0}}';

  /* 输入框前缀：用 Arco 描边锁，和后台其它图标同一套 */
  var LOCK_SVG = '<svg viewBox="0 0 48 48" fill="none" xmlns="http://www.w3.org/2000/svg"'
    + ' stroke="currentColor" class="arco-icon arco-icon-lock" stroke-width="4"'
    + ' stroke-linecap="butt" stroke-linejoin="miter"'
    + ' style="width:16px;height:16px;display:block">'
    + '<rect x="7" y="21" width="34" height="20" rx="1"></rect>'
    + '<path d="M15 21v-6a9 9 0 1 1 18 0v6M24 35v-8"></path></svg>';

  /* 菜单里用 20px + padding-right:5px，和旁边几个 Arco 图标一致 */
  var LOCK_SVG_MENU = '<svg viewBox="0 0 48 48" fill="none" xmlns="http://www.w3.org/2000/svg"'
    + ' stroke="currentColor" class="arco-icon arco-icon-lock" stroke-width="4"'
    + ' stroke-linecap="butt" stroke-linejoin="miter"'
    + ' style="font-size:20px;padding-right:5px">'
    + '<rect x="7" y="21" width="34" height="20" rx="1"></rect>'
    + '<path d="M15 21v-6a9 9 0 1 1 18 0v6M24 35v-8"></path></svg>';

  var CLOSE_SVG = '<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path fill="currentColor"'
    + ' d="M764.288 214.592 512 466.88 259.712 214.592a31.936 31.936 0 0 0-45.12 45.12L466.752 512'
    + ' 214.528 764.224a31.936 31.936 0 1 0 45.12 45.184L512 557.184l252.288 252.288a31.936 31.936 0 0 0'
    + ' 45.12-45.12L557.12 512.064l252.288-252.352a31.936 31.936 0 1 0-45.12-45.184z"></path></svg>';

  function mdyToast(msg) {
    var d = document.createElement('div');
    d.textContent = msg;
    d.style.cssText = 'position:fixed;top:24px;left:50%;transform:translateX(-50%);z-index:10000;'
      + 'background:#fff;border-radius:4px;box-shadow:0 4px 16px rgba(0,0,0,.16);'
      + 'padding:10px 18px;font-size:13px;color:#1d2129';
    document.body.appendChild(d);
    setTimeout(function () { if (d.parentNode) d.parentNode.removeChild(d); }, 2400);
  }

  function mdyField(id, label, ph) {
    return '<div class="mdy-fi"><label for="' + id + '">' + label + '</label>'
      + '<div class="el-input"><div class="el-input__wrapper">'
      + '<span class="el-input__prefix"><i class="el-icon">' + LOCK_SVG + '</i></span>'
      + '<input class="el-input__inner" id="' + id + '" type="password" '
      + 'autocomplete="new-password" placeholder="' + ph + '"></div></div></div>';
  }

  function closeResetModal() {
    var mask = document.getElementById('mdyPwdMask');
    if (!mask) return;
    mask.style.animation = 'mdyMaskOut .3s forwards';
    var box = mask.querySelector('.el-message-box');
    if (box) box.style.animation = 'msgbox-fade-out .3s forwards';
    setTimeout(function () { if (mask.parentNode) mask.parentNode.removeChild(mask); }, 300);
  }

  function submitPwd() {
    var cur = document.getElementById('mdyPwdCur').value;
    var np = document.getElementById('mdyPwdNew').value;
    var cf = document.getElementById('mdyPwdNew2').value;
    var err = document.getElementById('mdyPwdErr');
    err.textContent = '';
    if (!cur) { err.textContent = '请输入当前密码'; return; }
    if (np.length < 6) { err.textContent = '新密码至少 6 位'; return; }
    if (np !== cf) { err.textContent = '两次输入的新密码不一致'; return; }
    if (np === cur) { err.textContent = '新密码不能与当前密码相同'; return; }

    var btn = document.getElementById('mdyPwdOk');
    btn.disabled = true;
    var span = btn.querySelector('span');
    if (span) span.textContent = '提交中…';

    var x = new XMLHttpRequest();
    x.open('POST', API_BASE + '?__p=' + encodeURIComponent('/v1/changePassword'));
    x.setRequestHeader('Content-Type', 'application/json');
    x.onload = function () {
      btn.disabled = false;
      if (span) span.textContent = '确定';
      var r = {};
      try { r = JSON.parse(x.responseText); } catch (e) {}
      if (r && r.code === 200) {
        closeResetModal();
        mdyToast(r.message || '密码已修改');
      } else {
        err.textContent = (r && r.message) || '修改失败，请重试';
      }
    };
    x.onerror = function () {
      btn.disabled = false;
      if (span) span.textContent = '确定';
      err.textContent = '网络错误，请重试';
    };
    x.send(JSON.stringify({ current: cur, password: np }));
  }

  function openResetModal() {
    if (document.getElementById('mdyPwdMask')) return;

    var mask = document.createElement('div');
    mask.id = 'mdyPwdMask';
    mask.className = 'el-overlay';
    mask.style.zIndex = '3000';
    mask.innerHTML = ''
      + '<div class="el-message-box" role="dialog" aria-modal="true" tabindex="-1">'
      + '<div class="el-message-box__header">'
      + '<div class="el-message-box__title"><span>重置密码</span></div>'
      + '<button type="button" class="el-message-box__headerbtn" aria-label="关闭">'
      + '<i class="el-icon el-message-box__close">' + CLOSE_SVG + '</i></button>'
      + '</div>'
      + '<div class="el-message-box__content"><div class="el-message-box__container">'
      + '<div class="el-message-box__message">'
      + '<form id="mdyPwdForm" onsubmit="return false" style="margin:0">'
      + mdyField('mdyPwdCur', '当前密码', '请输入当前密码')
      + mdyField('mdyPwdNew', '新密码', '至少 6 位')
      + mdyField('mdyPwdNew2', '确认新密码', '请再次输入新密码')
      + '<div id="mdyPwdErr"></div>'
      + '</form></div></div></div>'
      + '<div class="el-message-box__btns">'
      + '<button type="button" class="el-button" id="mdyPwdCancel"><span>取消</span></button>'
      + '<button type="button" class="el-button el-button--primary" id="mdyPwdOk"><span>确定</span></button>'
      + '</div>';

    document.body.appendChild(mask);

    mask.addEventListener('click', function (e) {
      if (e.target === mask) closeResetModal();
    });
    mask.querySelector('.el-message-box__headerbtn').addEventListener('click', closeResetModal);
    document.getElementById('mdyPwdCancel').addEventListener('click', closeResetModal);
    document.getElementById('mdyPwdOk').addEventListener('click', submitPwd);

    var inputs = mask.querySelectorAll('input.el-input__inner');
    for (var i = 0; i < inputs.length; i++) {
      inputs[i].addEventListener('focus', function () {
        if (this.parentNode) this.parentNode.classList.add('is-focus');
      });
      inputs[i].addEventListener('blur', function () {
        if (this.parentNode) this.parentNode.classList.remove('is-focus');
      });
      inputs[i].addEventListener('keydown', function (e) {
        if (e.key === 'Enter') { e.preventDefault(); submitPwd(); }
        if (e.key === 'Escape') { closeResetModal(); }
      });
    }

    /* 和 ElMessageBox 一致：打开时 focus 确定按钮，按钮即呈现 :focus 态（浅色），
       点别处 / 点输入框后焦点移开，自动变回默认深色 */
    setTimeout(function () {
      var ok = document.getElementById('mdyPwdOk');
      if (ok) ok.focus();
    }, 30);
  }

  function injectResetMenuItem() {
    var ul = document.querySelector('ul.el-dropdown-menu');
    if (!ul || ul.querySelector('[data-mdy-reset]')) return;
    var items = ul.querySelectorAll('li.el-dropdown-menu__item');
    var anchor = null;
    for (var i = 0; i < items.length; i++) {
      if ((items[i].innerText || '').indexOf('清除缓存') >= 0) { anchor = items[i]; break; }
    }
    if (!anchor) return;
    var li = document.createElement('li');
    li.className = anchor.className;
    li.setAttribute('data-mdy-reset', '1');
    li.setAttribute('role', 'menuitem');
    li.setAttribute('tabindex', '-1');
    li.innerHTML = LOCK_SVG_MENU + ' 重置密码 ';
    li.addEventListener('click', function (e) {
      e.stopPropagation();
      var pop = document.querySelector('.el-dropdown__popper');
      if (pop) pop.style.display = 'none';
      openResetModal();
    });
    anchor.parentNode.insertBefore(li, anchor.nextSibling);
  }

  function boot() {
    injectCss();
    injectResetMenuItem();
    try {
      var x = new XMLHttpRequest();
      x.open('GET', API_BASE + '?__p=' + encodeURIComponent('/v1/status'));
      x.onload = function () {
        try {
          var r = JSON.parse(x.responseText);
          if (r && r.data && typeof r.data.hasAccount !== 'undefined') {
            hasAccount = !!r.data.hasAccount;
            applyFirstTimeUI();
          }
        } catch (e) {}
      };
      x.send();
    } catch (e) {}

    var t = setInterval(function () {
      applyFirstTimeUI();
      hideExtraLinks();
      injectResetMenuItem();
    }, 400);
    setTimeout(function () { clearInterval(t); }, 60000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
