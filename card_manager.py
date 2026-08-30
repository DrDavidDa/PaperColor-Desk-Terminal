# -*- coding: utf-8 -*-
"""
PaperColor 考点卡片 Web 管理器
浏览器编辑 cards/chNN.json → 保存本地 → 一键串口上传到设备 SD /cards/

用法:
    py -3 card_manager.py            # 默认 COM4, 打开 http://localhost:8765
    py -3 card_manager.py COM5 9000  # 指定串口与端口

零第三方后端依赖（仅 pyserial），上传协议与 upload_file.py 完全一致
（#FILEUPLOAD 分块 200B + ACK 流控）。上传完成后重启设备生效。
"""
import json
import os
import re
import sys
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

# 防止 Windows 控制台 GBK 编码崩溃（asrsim2 历史教训）
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

BASE = os.path.dirname(os.path.abspath(__file__))
CARDS_DIR = os.path.join(BASE, 'cards')
PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
WEB_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
SERIAL_LOCK = threading.Lock()   # 串口独占（Web 多线程并发上传保护）

CH_RE = re.compile(r'^ch(\d+)\.json$', re.I)


# ==================== 卡片存储 ====================

def chapter_files():
    """扫描 cards/ 下 chNN.json，返回 [(编号, 文件名, 卡片数), ...] 按编号排序"""
    out = []
    if os.path.isdir(CARDS_DIR):
        for name in os.listdir(CARDS_DIR):
            m = CH_RE.match(name)
            if m:
                path = os.path.join(CARDS_DIR, name)
                try:
                    cards = json.load(open(path, encoding='utf-8'))
                    out.append((int(m.group(1)), name, len(cards) if isinstance(cards, list) else 0))
                except Exception:
                    out.append((int(m.group(1)), name, -1))   # -1 = JSON 解析失败
    return sorted(out)


def read_cards(filename):
    path = os.path.join(CARDS_DIR, filename)
    if not os.path.isfile(path):
        return []
    return json.load(open(path, encoding='utf-8'))


def write_cards(filename, cards):
    if not CH_RE.match(filename):
        raise ValueError('文件名必须是 chNN.json 格式')
    if not os.path.isdir(CARDS_DIR):
        os.makedirs(CARDS_DIR)
    path = os.path.join(CARDS_DIR, filename)
    # 与现有 ch01.json 格式一致：中文不转义、缩进 1
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(cards, f, ensure_ascii=False, indent=1)
    return path


# ==================== 串口上传（协议同 upload_file.py） ====================

def wait_ack(s, ack, timeout):
    end = time.time() + timeout
    buf = b''
    while time.time() < end:
        d = s.read(512)
        if d:
            buf += d
            if ack in buf:
                return True
    return False


def upload_to_device(local_path, device_path, log):
    data = open(local_path, 'rb').read()
    with SERIAL_LOCK:
        # 打开串口会复位设备（dtr=True），随后等 boot 完成
        s = serial.Serial(PORT, 115200, timeout=0.5)
        time.sleep(0.3)
        try:
            ready = False
            for _ in range(20):
                s.reset_input_buffer()
                s.write(b'#STATUS\n')
                time.sleep(0.4)
                if s.read(256):
                    ready = True
                    break
            if not ready:
                raise RuntimeError('设备未就绪（无串口回复，检查 USB / 是否 light sleep 卡死需重插）')
            log('[upload] 设备就绪，等待轮询空闲...')
            # 先 #POLL 等智谱轮询完成，确保 loop 空闲（防 WiFi 阻塞串口溢出）
            s.reset_input_buffer()
            s.write(b'#POLL\n')
            end = time.time() + 75
            while time.time() < end:
                d = s.read(512)
                if d and '今日token'.encode('utf-8') in d:
                    break
            # 发起上传
            s.reset_input_buffer()
            s.write(('#FILEUPLOAD|%s|%d\n' % (device_path, len(data))).encode())
            if not wait_ack(s, b'file start', 10):
                raise RuntimeError('未收到 file start（SD 卡/路径问题）')
            CHUNK, GAP = 200, 0.03
            sent = 0
            for i in range(0, len(data), CHUNK):
                s.write(data[i:i + CHUNK])
                sent += len(data[i:i + CHUNK])
                time.sleep(GAP)
                if sent % 2000 < CHUNK:
                    log('[upload] %d/%d 字节' % (sent, len(data)))
            if not wait_ack(s, b'file done', 10):
                raise RuntimeError('未收到 file done（传输中断）')
            log('[upload] 完成: %s（%d 字节）→ 重启设备后生效' % (device_path, len(data)))
        finally:
            s.close()


# ==================== Web UI ====================

PAGE = '''<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>小彩 · 考点卡片管理器</title>
<style>
  :root { --bg:#f5f7fa; --card:#fff; --ink:#1a2233; --sub:#5b6b83; --line:#e3e9f2;
          --blue:#2563eb; --green:#059669; --red:#dc2626; --amber:#d97706; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font-family:"Segoe UI","Microsoft YaHei",sans-serif; background:var(--bg); color:var(--ink); }
  header { background:linear-gradient(135deg,#1e3a8a,#2563eb); color:#fff; padding:18px 28px;
           display:flex; align-items:center; gap:14px; }
  header h1 { font-size:20px; font-weight:600; }
  header .tag { font-size:12px; opacity:.85; background:rgba(255,255,255,.15); padding:3px 10px; border-radius:99px; }
  main { max-width:1180px; margin:24px auto; padding:0 20px; display:grid;
         grid-template-columns:260px 1fr; gap:20px; }
  @media (max-width:760px){ main { grid-template-columns:1fr; } }
  .panel { background:var(--card); border:1px solid var(--line); border-radius:14px; overflow:hidden; }
  .panel h2 { font-size:14px; color:var(--sub); padding:12px 16px; border-bottom:1px solid var(--line);
              background:#fafbfe; letter-spacing:.5px; }
  .chlist { list-style:none; max-height:520px; overflow:auto; }
  .chlist li { padding:11px 16px; border-bottom:1px solid var(--line); cursor:pointer;
               display:flex; justify-content:space-between; align-items:center; font-size:14px; }
  .chlist li:hover { background:#f0f5ff; }
  .chlist li.active { background:#eef2ff; color:var(--blue); font-weight:600; }
  .chlist .n { font-size:12px; color:var(--sub); background:var(--bg); border-radius:99px; padding:1px 9px; }
  .chlist .bad .n { background:#fee2e2; color:var(--red); }
  .pad { padding:16px; }
  button { border:0; border-radius:9px; padding:9px 16px; font-size:14px; cursor:pointer;
           background:var(--blue); color:#fff; transition:.15s; }
  button:hover { filter:brightness(1.08); }
  button:disabled { opacity:.5; cursor:not-allowed; }
  button.ghost { background:var(--bg); color:var(--ink); border:1px solid var(--line); }
  button.green { background:var(--green); }
  button.red { background:#fff; color:var(--red); border:1px solid #fecaca; }
  button.sm { padding:5px 12px; font-size:13px; }
  .cards { display:flex; flex-direction:column; gap:12px; }
  .card { border:1px solid var(--line); border-radius:12px; padding:14px 16px; background:#fdfefe; }
  .card .head { display:flex; gap:10px; margin-bottom:10px; align-items:center; }
  .card .head input { flex:1; }
  .card textarea, .card input { width:100%; border:1px solid var(--line); border-radius:8px;
      padding:8px 10px; font-size:14px; font-family:inherit; resize:vertical; background:#fff; }
  .card textarea:focus, .card input:focus { outline:2px solid #bfdbfe; }
  .card label { font-size:12px; color:var(--sub); display:block; margin:8px 0 4px; }
  .row2 { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
  .toolbar { display:flex; gap:10px; margin-bottom:14px; flex-wrap:wrap; align-items:center; }
  .toolbar input { border:1px solid var(--line); border-radius:8px; padding:8px 10px; width:130px; }
  #log { font-family:Consolas,monospace; font-size:12.5px; color:var(--sub); white-space:pre-wrap;
         max-height:130px; overflow:auto; background:#0f172a; color:#a5f3fc; border-radius:10px;
         padding:10px 14px; margin-top:14px; display:none; }
  .empty { text-align:center; color:var(--sub); padding:40px 0; font-size:14px; }
</style>
</head>
<body>
<header>
  <h1>📟 小彩 · 考点卡片管理器</h1>
  <span class="tag" id="port">串口: __PORT__</span>
  <span class="tag">编辑 → 保存 → 上传 → 重启设备生效</span>
</header>
<main>
  <div class="panel">
    <h2>章 节</h2>
    <ul class="chlist" id="chlist"></ul>
    <div class="pad">
      <div class="toolbar" style="margin:0">
        <input id="newch" type="number" min="1" max="99" placeholder="章节号">
        <button class="ghost sm" onclick="newChapter()">新建章节</button>
      </div>
    </div>
  </div>
  <div class="panel">
    <h2 id="cur">卡 片</h2>
    <div class="pad">
      <div class="toolbar">
        <button onclick="addCard()">＋ 新增卡片</button>
        <button class="green" id="save" onclick="saveAll()">💾 保存到本地</button>
        <button class="ghost" id="up" onclick="uploadDev()">⬆ 上传到设备</button>
        <span id="st" style="font-size:13px;color:var(--sub)"></span>
      </div>
      <div class="cards" id="cards"><div class="empty">← 选择或新建章节</div></div>
    </div>
  </div>
</main>
<div style="max-width:1180px;margin:0 auto 30px;padding:0 20px;"><div id="log"></div></div>
<script>
let ch = null, cards = [];

function log(m){ const el=document.getElementById('log'); el.style.display='block';
  el.textContent += m+'\\n'; el.scrollTop = el.scrollHeight; }
async function jget(u){ const r = await fetch(u); if(!r.ok) throw new Error(await r.text()); return r.json(); }
async function jpost(u,b){ const r = await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const t = await r.text(); if(!r.ok) throw new Error(t); return t?JSON.parse(t):{}; }

async function refreshChapters(){
  const list = await jget('/api/chapters');
  const ul = document.getElementById('chlist'); ul.innerHTML='';
  if(!list.length){ ul.innerHTML='<li class="empty">暂无章节</li>'; return; }
  for(const c of list){
    const li = document.createElement('li');
    li.className = (ch===c.file?'active ':'') + (c.count<0?'bad':'');
    li.innerHTML = `<span>${c.file}</span><span class="n">${c.count<0?'JSON错误':c.count+' 张'}</span>`;
    li.onclick = ()=>openCh(c.file);
    ul.appendChild(li);
  }
}
async function openCh(f){
  ch = f; cards = await jget('/api/cards?ch='+encodeURIComponent(f));
  document.getElementById('cur').textContent = '卡 片 · '+f;
  renderCards(); refreshChapters();
}
function renderCards(){
  const box = document.getElementById('cards');
  if(!cards.length){ box.innerHTML='<div class="empty">空章节，点「新增卡片」</div>'; return; }
  box.innerHTML = cards.map((c,i)=>`
  <div class="card">
    <div class="head">
      <b style="font-size:13px;color:var(--sub)">#${i+1}</b>
      <input value="${esc(c.category||'')}" placeholder="分类（如：经济基础）" oninput="cards[${i}].category=this.value">
      <button class="red sm" onclick="delCard(${i})">删除</button>
    </div>
    <label>问题</label>
    <textarea rows="2" oninput="cards[${i}].question=this.value">${esc(c.question||'')}</textarea>
    <div class="row2">
      <div><label>答案</label><textarea rows="2" oninput="cards[${i}].answer=this.value">${esc(c.answer||'')}</textarea></div>
      <div><label>速记口诀</label><textarea rows="2" oninput="cards[${i}].mnemonic=this.value">${esc(c.mnemonic||'')}</textarea></div>
    </div>
  </div>`).join('');
  document.getElementById('st').textContent = cards.length+' 张';
}
function esc(s){ return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;'); }
function addCard(){ cards.push({category:'',question:'',answer:'',mnemonic:''}); renderCards(); }
function delCard(i){ if(confirm('删除第 '+(i+1)+' 张卡片？')){ cards.splice(i,1); renderCards(); } }
async function saveAll(){
  try{ await jpost('/api/save',{ch:ch,cards:cards}); document.getElementById('st').textContent='✅ 已保存 '+cards.length+' 张';
    log('[save] '+ch+' 已保存'); refreshChapters(); }
  catch(e){ alert('保存失败: '+e.message); }
}
async function uploadDev(){
  if(!ch) return;
  await saveAll();
  const b=document.getElementById('up'); b.disabled=true; log('[upload] 开始上传 '+ch+' ...');
  try{ const r = await jpost('/api/upload',{ch:ch}); (r.log||[]).forEach(log); alert('上传完成！重启设备后生效。'); }
  catch(e){ log('[upload] 失败: '+e.message); alert('上传失败: '+e.message); }
  b.disabled=false;
}
async function newChapter(){
  const n = parseInt(document.getElementById('newch').value);
  if(!n||n<1||n>99){ alert('章节号 1-99'); return; }
  const f = 'ch'+String(n).padStart(2,'0')+'.json';
  await jpost('/api/save',{ch:f,cards:[]}); openCh(f);
}
refreshChapters();
</script>
</body>
</html>'''


# ==================== HTTP 服务 ====================

class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype='application/json; charset=utf-8'):
        data = body.encode('utf-8') if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            self._send(200, PAGE.replace('__PORT__', PORT), 'text/html; charset=utf-8')
        elif self.path == '/api/chapters':
            self._send(200, json.dumps(
                [{'file': name, 'count': n} for _, name, n in chapter_files()],
                ensure_ascii=False))
        elif self.path.startswith('/api/cards'):
            qs = dict(p.split('=', 1) for p in self.path.split('?', 1)[1].split('&')) if '?' in self.path else {}
            fn = os.path.basename(qs.get('ch', ''))
            try:
                self._send(200, json.dumps(read_cards(fn), ensure_ascii=False))
            except Exception as e:
                self._send(500, json.dumps({'error': str(e)}, ensure_ascii=False))
        else:
            self._send(404, '{"error":"not found"}')

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            body = json.loads(self.rfile.read(length).decode('utf-8')) if length else {}
        except Exception as e:
            self._send(400, json.dumps({'error': 'bad json: %s' % e}, ensure_ascii=False))
            return

        if self.path == '/api/save':
            try:
                fn = os.path.basename(str(body.get('ch', '')))
                if not CH_RE.match(fn):
                    raise ValueError('章节文件名必须是 chNN.json')
                cards = body.get('cards', [])
                for c in cards:   # 字段白名单，防脏数据
                    c.setdefault('category', ''); c.setdefault('question', '')
                    c.setdefault('answer', ''); c.setdefault('mnemonic', '')
                write_cards(fn, cards)
                self._send(200, json.dumps({'ok': True, 'count': len(cards)}, ensure_ascii=False))
            except Exception as e:
                self._send(500, json.dumps({'error': str(e)}, ensure_ascii=False))

        elif self.path == '/api/upload':
            fn = os.path.basename(str(body.get('ch', '')))
            if not CH_RE.match(fn):
                self._send(400, json.dumps({'error': 'bad chapter'}, ensure_ascii=False))
                return
            logs = []

            def log(m):
                logs.append(m)
                print(m, flush=True)
            try:
                upload_to_device(os.path.join(CARDS_DIR, fn), '/cards/' + fn, log)
                self._send(200, json.dumps({'ok': True, 'log': logs}, ensure_ascii=False))
            except Exception as e:
                logs.append('[upload] 失败: %s' % e)
                self._send(500, json.dumps({'error': str(e), 'log': logs}, ensure_ascii=False))
        else:
            self._send(404, '{"error":"not found"}')

    def log_message(self, *a):   # 静默默认访问日志
        pass


def main():
    print('PaperColor 卡片管理器  串口=%s  Web=http://localhost:%d' % (PORT, WEB_PORT), flush=True)
    print('章节目录: %s' % CARDS_DIR, flush=True)
    srv = ThreadingHTTPServer(('127.0.0.1', WEB_PORT), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print('\n退出')


if __name__ == '__main__':
    main()
