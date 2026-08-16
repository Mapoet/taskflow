#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
namespace bg = boost::geometry;
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using MultiPolygon = bg::model::multi_polygon<Polygon>;

// 用户定义 boost::throw_exception
namespace boost {
void throw_exception(std::exception const& e) {
    std::abort();
}
}

int main() {
    Polygon a, b;
    bg::append(a, Point(0,0)); bg::append(a, Point(4,0)); bg::append(a, Point(4,4)); bg::append(a, Point(0,4)); bg::correct(a);
    bg::append(b, Point(2,2)); bg::append(b, Point(6,2)); bg::append(b, Point(6,6)); bg::append(b, Point(2,6)); bg::correct(b);
    MultiPolygon inter;
    bg::intersection(a, b, inter);
    std::cout << "交集: " << bg::wkt(inter) << "\n";
    std::cout << "面积: " << bg::area(inter) << "\n";
    return 0;
}
