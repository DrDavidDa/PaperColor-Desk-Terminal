# -*- coding: utf-8 -*-
# 一键发布固件到 M5Burner（全自动：登录→上传→公开→Share Code）
# 逆向自 M5Burner v3 客户端（packages/view main.js + app.asar packages/auth.js）：
#   登录:   POST https://uiflow2.m5stack.com/api/v1/account/login {email,password}
#           → Set-Cookie m5_auth_token=<token>（即 ssoToken）+ body.data.username
#   发布:   POST http://m5burner-api.m5stack.com/api/admin/firmware
#           multipart: name/description/category/author/version/github/cover/firmware
#           header: m5_auth_token=<token>
#   公开:   PUT  {API}/api/admin/firmware/{fid}/publish/{version}/1
#   分享码: POST {API}/api/admin/firmware/share/{fid}/{version}
# 用法:
#   1. 在 m5_credentials.txt 写两行：第1行邮箱 第2行密码（已被 .gitignore 排除）
#   2. python publish_m5burner.py
# 镜像合成（如尚未生成）:
#   python merge_firmware_image.py
import requests, sys, os, json

UIFLOW_HOST = 'https://uiflow2.m5stack.com'
API = 'http://m5burner-api.m5stack.com'
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(HERE, 'm5burner_image.bin')
COVER = os.path.join(HERE, 'docs', 'images', 'standby.jpg')
CREDS = os.path.join(HERE, 'm5_credentials.txt')

NAME = 'eInk Desk Terminal'
VERSION = 'v1.0.0'
CATEGORY = 'paper'          # M5Burner 官方 PaperColor 类别
GITHUB = 'https://github.com/DrDavidDa/eInk-Desk-Terminal'
DESCRIPTION = ('Full-featured eInk desk study terminal for M5Stack PaperColor '
               '(ESP32-S3 + 4" Spectra 6 full-color E-Ink). 7 pages: lunar calendar '
               '+ weather, tech news RSS, Anki-style flash cards with spaced repetition, '
               'Zhipu CodingPlan quota dashboard, voice memo with offline Chinese TTS, '
               'environment dashboard, QR viewer. Offline Chinese TTS + global voice '
               'assistant "Xiaocai" (long-press C). Ultra-low-power standby ~2mA. '
               'First boot: write WiFi config to SD /config.ini (see GitHub README).')


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


def set_public(token, fid, version):
    r = requests.put(API + '/api/admin/firmware/%s/publish/%s/1' % (fid, version),
                     json={}, headers={'m5_auth_token': token}, timeout=30)
    print('[%s] 公开 HTTP %d %s' % ('OK' if r.status_code == 200 else '??', r.status_code, r.text[:200]))


def share_code(token, fid, version):
    r = requests.post(API + '/api/admin/firmware/share/%s/%s' % (fid, version),
                      json={}, headers={'m5_auth_token': token}, timeout=30)
    print('[%s] ShareCode HTTP %d: %s' % ('OK' if r.status_code == 200 else '??', r.status_code, r.text[:300]))
    return r.text


def main():
    email, password = read_creds()
    token, username = login(email, password)

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

    set_public(token, fid, VERSION)
    share_code(token, fid, VERSION)
    print('[DONE] 全流程完成：固件已发布并公开')


if __name__ == '__main__':
    main()
