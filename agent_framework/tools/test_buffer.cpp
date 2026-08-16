#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
#include <iomanip>
namespace bg = boost::geometry;
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using MultiPolygon = bg::model::multi_polygon<Polygon>;
namespace boost { void throw_exception(std::exception const&) { std::abort(); } }
int main() {
    Polygon sq;
    bg::append(sq, Point(0,0)); bg::append(sq, Point(2,0)); bg::append(sq, Point(2,2)); bg::append(sq, Point(0,2)); bg::correct(sq);
    MultiPolygon buf;
    bg::buffer(sq, buf, bg::strategy::buffer::distance_symmetric<double>(1.0),
               bg::strategy::buffer::side_straight(),
               bg::strategy::buffer::join_round(12),
               bg::strategy::buffer::end_round(12),
               bg::strategy::buffer::point_circle(12));
    std::cout << "缓冲面积: " << std::fixed << std::setprecision(4) << bg::area(buf) << "\n";
    return 0;
}
