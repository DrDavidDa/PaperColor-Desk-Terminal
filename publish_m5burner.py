# -*- coding: utf-8 -*-
# 一键发布固件到 M5Burner（全自动：登录→上传→公开→Share Code）
# 逆向自 M5Burner v3 客户端（packages/view main.js + app.asar packages/auth.js）：
#   登录:   POST https://uiflow2.m5stack.com/api/v1/account/login {email,password}
#           → Set-Cookie m5_auth_token=<token>（即 ssoToken）+ body.data.username
#   发布:   POST http://m5burner-api.m5stack.com/api/admin/firmware
#           multipart: name/description/category/author/version/github/cover/firmware
#           header: m5_auth_token=<token>
#   更新:   PUT  {API}/api/admin/firmware/{fid}/{versionInfo.file}
#           multipart: name/description/category/author/version/github + 可选 cover/firmware
#           （cover、firmware 不传则保留原值；fid 不变 → 分享码不失效，无需重传 16MB）
#   公开:   PUT  {API}/api/admin/firmware/{fid}/publish/{version}/1
#   分享码: POST {API}/api/admin/firmware/share/{fid}/{version}
# 用法:
#   1. 在 m5_credentials.txt 写两行：第1行邮箱 第2行密码（已被 .gitignore 排除）
#   2. python publish_m5burner.py            # 全量上传新固件
#      python publish_m5burner.py --meta-only  # 只更新名称/介绍/封面（不动 16MB 固件）
# 镜像合成（如尚未生成）:
#   python merge_firmware_image.py
import requests, sys, os, json

UIFLOW_HOST = 'https://uiflow2.m5stack.com'
API = 'http://m5burner-api.m5stack.com'
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(HERE, 'm5burner_image.bin')
COVER = os.path.join(HERE, 'docs', 'images', 'cover.gif')   # 轮播动图封面（make_cover.py 生成）
CREDS = os.path.join(HERE, 'm5_credentials.txt')

NAME = 'PaperColor eInk Desk Terminal'
VERSION = 'v1.0.0'
CATEGORY = 'paper'          # M5Burner 官方 PaperColor 类别
GITHUB = 'https://github.com/DrDavidDa/PaperColor-Desk-Terminal'
FID = '0fc711c40244fb17a7df65c434deda4d'   # 首次上传分配的固件 ID（meta-only 按它定位）
DESCRIPTION = (
    '🔥 M5Stack PaperColor 全彩墨水屏「完全体」固件：黄历天气 · 科技早报 · Anki 考点闪卡 · '
    '番茄钟 · 语音待办 · 环境仪表盘，7 页桌面终端一屏掌控！\n'
    '🎙 全离线中文 TTS + 全局语音助手「小彩」：任意页长按 C，说「打开日历」「开始番茄」，'
    '一句话直达，彻底解放双手。\n'
    '🌙 真·2mA 超低功耗待机，续航 25~50 天；墨水屏不闪烁、不烧屏，摆桌上常亮一整年。\n'
    '⚡ M5Burner 一键烧录，SD 卡写入 WiFi 即联网，开箱即玩！\n'
    'English: Turn your 4" Spectra 6 full-color eInk into an all-in-one desk terminal — '
    'lunar calendar & weather, tech news RSS, spaced-repetition flashcards, Pomodoro, '
    'voice memos & dashboard. Fully offline Chinese TTS + voice assistant (long-press C), '
    '~2mA ultra-low-power standby. Burn once, enjoy for years. 🚀')


def read_creds():
    if not os.path.exists(CREDS):
        print('[FAIL] 缺少 %s（第1行邮箱，第2行密码）' % CREDS)
        sys.exit(1)
    lines = [l.strip() for l in open(CREDS, encoding='utf-8').read().splitlines() if l.strip()]
    if len(lines) < 2:
        print('[FAIL] m5_credentials.txt 需两行：邮箱 / 密码')
        sys.exit(1)
    return lines[0], lines[1]


def login(email, password):
    r = requests.post(UIFLOW_HOST + '/api/v1/account/login',
                      json={'email': email, 'password': password}, timeout=30)
    if r.status_code != 200:
        print('[FAIL] 登录 HTTP %d: %s' % (r.status_code, r.text[:200]))
        sys.exit(1)
    token = r.cookies.get('m5_auth_token')
    if not token:
        for c in r.headers.get('set-cookie', '').split(';'):
            if 'm5_auth_token=' in c:
                token = c.split('m5_auth_token=')[1].strip()
                break
    if not token:
        print('[FAIL] 登录成功但未返回 m5_auth_token：', r.text[:300])
        sys.exit(1)
    username = ''
    try:
        username = r.json()['data']['username']
    except Exception:
        pass
    print('[OK] 登录成功 user=%s token=%s...%s' % (username, token[:8], token[-6:]))
    return token, username


def publish(token, username):
    if not os.path.exists(IMAGE):
        print('[FAIL] 缺少镜像 %s，先运行 python merge_firmware_image.py' % IMAGE)
        sys.exit(1)
    data = {
        'name': NAME,
        'description': DESCRIPTION,
        'category': CATEGORY,
        'author': username or 'DrDavidDa',
        'version': VERSION,
        'github': GITHUB,
    }
    files = {
        'cover': ('cover.jpg', open(COVER, 'rb').read(), 'image/jpeg'),
        'firmware': ('firmware.bin', open(IMAGE, 'rb').read(), 'application/octet-stream'),
    }
    print('[..] 上传 16MB 镜像到 %s ...' % API)
    r = requests.post(API + '/api/admin/firmware', data=data, files=files,
                      headers={'m5_auth_token': token}, timeout=600)
    print('[..] HTTP %d' % r.status_code)
    try:
        print(json.dumps(r.json(), ensure_ascii=False)[:800])
        return r.json()
    except Exception:
        print(r.text[:800])
        return None


def get_own(token):
    r = requests.get(API + '/api/admin/firmware',
                     headers={'m5_auth_token': token}, timeout=30)
    try:
        data = r.json()
    except Exception:
        print(r.text[:500])
        return None
    # 兼容裸列表 / {data:[...]} / {data:{list:[...]}} 三种包法
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        inner = data.get('data', data)
        if isinstance(inner, list):
            return inner
        if isinstance(inner, dict) and isinstance(inner.get('list'), list):
            return inner['list']
    return data


def set_public(token, fid, file_hash):
    # 注意：端点第2参数是 versionInfo.file（内部文件名hash.bin），不是版本字符串（逆向 UI 源码确认）
    r = requests.put(API + '/api/admin/firmware/%s/publish/%s/1' % (fid, file_hash),
                     json={}, headers={'m5_auth_token': token}, timeout=30)
    print('[%s] 公开 HTTP %d %s' % ('OK' if r.status_code == 200 else '??', r.status_code, r.text[:200]))


def share_code(token, fid, version):
    r = requests.post(API + '/api/admin/firmware/share/%s/%s' % (fid, version),
                      json={}, headers={'m5_auth_token': token}, timeout=30)
    print('[%s] ShareCode HTTP %d: %s' % ('OK' if r.status_code == 200 else '??', r.status_code, r.text[:300]))
    return r.text


def update_meta(token):
    """只更新名称/介绍/封面（fid 不变，分享码不失效，不重传 16MB 固件）"""
    own = get_own(token)
    item = None
    for it in (own or []):
        if (it.get('fid') or it.get('_id')) == FID:
            item = it
            break
    if not item:
        print('[FAIL] 未找到 fid=%s，先全量上传一次' % FID)
        sys.exit(1)
    vers = item.get('versions') or []
    if not vers:
        print('[FAIL] 固件无版本信息')
        sys.exit(1)
    file_hash = vers[-1]['file']
    data = {
        'name': NAME,
        'description': DESCRIPTION,
        'category': CATEGORY,
        'author': item.get('author') or 'DrDavidDa',
        'version': vers[-1].get('version') or VERSION,
        'github': GITHUB,
    }
    files = {'cover': ('cover.gif', open(COVER, 'rb').read(), 'image/gif')}
    print('[..] 更新元数据 fid=%s file=%s ...' % (FID, file_hash))
    r = requests.put(API + '/api/admin/firmware/%s/version/%s' % (FID, file_hash),
                     data=data, files=files,
                     headers={'m5_auth_token': token}, timeout=120)
    print('[%s] 更新 HTTP %d %s' % ('OK' if r.status_code == 200 else 'FAIL', r.status_code, r.text[:300]))
    if r.status_code != 200:
        sys.exit(1)
    # 复核：重新拉列表确认新名称/封面已生效
    own = get_own(token)
    for it in (own or []):
        if (it.get('fid') or it.get('_id')) == FID:
            print('[OK] name=%s' % it.get('name'))
            print('[OK] cover=%s' % it.get('cover'))
            print('[OK] github=%s' % it.get('github'))
            return


def main():
    email, password = read_creds()
    token, username = login(email, password)

    # --meta-only：只改名/介绍/封面后直接结束
    if '--meta-only' in sys.argv:
        update_meta(token)
        print('[DONE] 元数据更新完成')
        return

    # 已存在则跳过重复上传（按名称在"我的固件"里查）
    own = get_own(token)
    fid = None
    if isinstance(own, list):
        for item in own:
            if item.get('name') == NAME:
                fid = item.get('fid') or item.get('_id')
                print('[OK] 固件已存在 fid=%s（跳过上传）' % fid)
                break
    else:
        print('[??] 我的固件响应结构未知：', str(own)[:400])
    if not fid:
        resp = publish(token, username)
        own = get_own(token)
        if isinstance(own, list):
            for item in own:
                if item.get('name') == NAME:
                    fid = item.get('fid') or item.get('_id')
                    print('[OK] 上传成功 fid=%s versions=%s' % (fid, item.get('versions')))
                    break
        if not fid:
            print('[FAIL] 上传后在"我的固件"中未找到，手动核对响应：', str(resp)[:400])
            sys.exit(1)

    # 公开端点的第2参数是内部文件名（fid+file 定位具体版本），从"我的固件"里取最新版本
    own = get_own(token)
    file_hash = None
    if isinstance(own, list):
        for item in own:
            if (item.get('fid') or item.get('_id')) == fid:
                vers = item.get('versions') or []
                if vers:
                    file_hash = vers[-1].get('file')
                break
    if not file_hash:
        print('[FAIL] 未取到版本内部文件名')
        sys.exit(1)
    set_public(token, fid, file_hash)
    share_code(token, fid, file_hash)
    print('[DONE] 全流程完成：固件已发布并公开')


if __name__ == '__main__':
    main()
