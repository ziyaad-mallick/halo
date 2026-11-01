from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'src/launcher.cpp').read_text(encoding='utf-8')
checks={'separate radio settings control':'Kind::CcRadioSettings' in s,'wifi settings destination':'L"ms-settings:network-wifi"' in s,'bluetooth settings destination':'L"ms-settings:bluetooth"' in s}
for k,v in checks.items(): print(('PASS ' if v else 'FAIL ')+k)
raise SystemExit(0 if all(checks.values()) else 1)
