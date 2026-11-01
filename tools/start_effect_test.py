from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/launcher.cpp').read_text(encoding='utf-8')
body=s.split('void Launcher::OpenMotion(',1)[1].split('// HALO 2.0: while hidden',1)[0]
ok='motion::In(' in body and 'motion::Out(' in body
print('Start delegates selected effects to shared motion:', 'PASS' if ok else 'FAIL')
raise SystemExit(0 if ok else 1)
