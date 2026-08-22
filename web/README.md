# NU Home Lab 웹앱

NU40-DK BLE 센서 리모컨으로 가상 주택의 조명을 제어하는 로컬 웹앱이다.

```bash
npm install
npm run dev
```

Chrome 또는 Edge에서 `http://localhost:3000`을 연다. 마우스 조작은 보드
연결 없이도 가능하다. 센서 리모컨은 `Bluetooth 연결`을 누르고
`NU40 Home Remote`를 선택한다.

```bash
npm run build
npm test
```

전체 설명은 상위 폴더의 [`docs/`](../docs/README.md)를 참고한다.
