"""FastAPI app for uvicorn: pydantic models in and out."""
from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI()


class Item(BaseModel):
    id: int
    name: str
    price: float
    tags: list[str] = []


DB = [Item(id=i, name='item%d' % i, price=i * 1.5, tags=['x', 'y']) for i in range(20)]


@app.get('/items/{iid}')
async def get_item(iid: int, q: str | None = None) -> Item:
    return DB[iid]


@app.get('/items')
async def items() -> list[Item]:
    return DB


@app.post('/items')
async def create(item: Item) -> Item:
    return item
