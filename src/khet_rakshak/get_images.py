# KHET-RAKSHAK: download training images
# Run:  pip install icrawler   then   python get_images.py
# Images land in ./dataset/<label>/ . Delete wrong ones (cartoons, logos, text) before uploading.
import os

from icrawler.builtin import BingImageCrawler

PER_QUERY = 40

classes = {
    "wild_boar": [
        "wild boar",
        "wild boar in field india",
        "wild boar camera trap night",
        "wild pig herd",
    ],
    "nilgai": ["nilgai", "nilgai blue bull india field", "nilgai camera trap night"],
    "monkey": [
        "rhesus macaque monkey",
        "langur monkey india",
        "monkey in farm india",
        "monkey camera trap night",
    ],
    "deer": ["spotted deer chital", "sambar deer", "deer camera trap night"],
    "safe": [
        "indian farmer walking in field",
        "person walking cctv night",
        "village dog india",
        "indian cow in field",
        "goat in indian village",
        "empty farm field india",
        "crop field at night",
    ],
}

os.makedirs("dataset", exist_ok=True)

for label, queries in classes.items():
    os.makedirs(f"dataset/{label}", exist_ok=True)
    for q in queries:
        print(f"[{label}] {q}")
        crawler = BingImageCrawler(
            storage={"root_dir": f"dataset/{label}"}, log_level=40
        )
        crawler.crawl(keyword=q, max_num=PER_QUERY, file_idx_offset="auto")
print("Done. Now open the dataset folder and delete bad images.")
