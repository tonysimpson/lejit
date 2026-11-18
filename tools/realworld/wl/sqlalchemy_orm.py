"""SQLAlchemy ORM: declarative models with a relationship, bulk unit-of-work inserts,
selectinload queries and updates, on sqlite in memory."""
import hashlib
from sqlalchemy import ForeignKey, String, create_engine, select
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column, relationship, selectinload


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = 'u'
    id: Mapped[int] = mapped_column(primary_key=True)
    name: Mapped[str] = mapped_column(String(50))
    addresses: Mapped[list['Address']] = relationship(back_populates='user')


class Address(Base):
    __tablename__ = 'a'
    id: Mapped[int] = mapped_column(primary_key=True)
    email: Mapped[str]
    user_id: Mapped[int] = mapped_column(ForeignKey('u.id'))
    user: Mapped[User] = relationship(back_populates='addresses')


e = create_engine('sqlite://')
Base.metadata.create_all(e)
with Session(e) as s:
    for i in range(10000):
        u = User(name='u%d' % i)
        u.addresses = [Address(email='a%d.%d@x' % (i, j)) for j in range(3)]
        s.add(u)
    s.commit()
h = hashlib.sha256()
for k in range(5):
    with Session(e) as s:
        for u in s.scalars(select(User).options(selectinload(User.addresses)).where(User.name.like('u1%')).order_by(User.id)):
            h.update(('%s:%s;' % (u.name, ','.join(a.email for a in u.addresses))).encode())
        for i in range(500):
            u = s.get(User, i + 1)
            u.name = u.name + 'x'
        s.commit()
print('rows', h.hexdigest())
