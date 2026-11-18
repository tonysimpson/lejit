"""Work already in C: sorted and list.sort with key functions, itemgetter and attrgetter, bisect and heapq over records."""
import bisect
import heapq
import operator


class Employee:
    __slots__ = ('name', 'dept', 'salary', 'age')

    def __init__(self, name, dept, salary, age):
        self.name = name
        self.dept = dept
        self.salary = salary
        self.age = age


DEPTS = ['eng', 'ops', 'sales', 'hr', 'legal']


def staff(n):
    return [Employee('e%05d' % (i * 7919 % 100000), DEPTS[i % 5], 20000 + i * 104729 % 80000, 20 + i % 45)
            for i in range(n)]


def main(n):
    people = staff(n)
    rows = [(p.name, p.dept, p.salary, p.age) for p in people]
    by_salary = sorted(people, key=operator.attrgetter('salary'))
    by_dept = sorted(rows, key=operator.itemgetter(1, 2))
    by_lambda = sorted(people, key=lambda p: (p.dept, -p.age, p.name))
    rows.sort(key=lambda r: r[0], reverse=True)
    ages = sorted(p.age for p in people)
    ladder = []
    for p in people[:2000]:
        bisect.insort(ladder, p.salary)
    top = heapq.nlargest(10, people, key=lambda p: p.salary)
    young = heapq.nsmallest(5, rows, key=operator.itemgetter(3, 0))
    return (by_salary[0].name, by_dept[-1], by_lambda[n // 2].name, rows[0][0],
            ages[n // 3], ladder[len(ladder) // 2], [p.name for p in top][:3], young[0])


N = 60000

if __name__ == '__main__':
    print(main(N))
