"""Explicit online preparation only; the runtime never downloads a model."""
import argparse
from pathlib import Path

REPOSITORY = "Systran/faster-whisper-small"
REVISION = "536b0662742c02347bc0e980a01041f333bce120"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=Path(__file__).parent / "models" / "whisper-small")
    args = parser.parse_args()
    from huggingface_hub import snapshot_download
    folder = snapshot_download(REPOSITORY, revision=REVISION, local_dir=str(args.destination),
                               allow_patterns=["model.bin", "config.json", "tokenizer.json", "preprocessor_config.json", "vocabulary.*"])
    print(f"로컬 모델 준비 완료: {folder}")
    print("이후 실행은 인터넷 연결 없이 가능합니다.")


if __name__ == "__main__":
    main()
