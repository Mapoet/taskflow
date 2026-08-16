#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
#include <iomanip>
namespace bg=boost::geometry;
using P=bg::model::d2::point_xy<double>;
using Poly=bg::model::polygon<P>;
using MP=bg::model::multi_polygon<Poly>;
namespace boost{void throw_exception(std::exception const&){std::abort();}}
int main(){
    Poly A,B;
    bg::append(A,P(0,0));bg::append(A,P(4,0));bg::append(A,P(4,4));bg::append(A,P(0,4));bg::correct(A);
    bg::append(B,P(2,2));bg::append(B,P(6,2));bg::append(B,P(6,6));bg::append(B,P(2,6));bg::correct(B);
    std::cout<<std::fixed<<std::setprecision(4);
    MP R;
    bg::intersection(A,B,R);
    std::cout<<"交集面积="<<bg::area(R)<<" (期望4)\n";
    bg::union_(A,B,R);
    std::cout<<"并集面积="<<bg::area(R)<<" (期望28)\n";
    bg::difference(A,B,R);
    std::cout<<"差集A-B面积="<<bg::area(R)<<" (期望12)\n";
    bg::difference(B,A,R);
    std::cout<<"差集B-A面积="<<bg::area(R)<<" (期望12)\n";
    bg::sym_difference(A,B,R);
    std::cout<<"对称差面积="<<bg::area(R)<<" (期望24)\n";
    return 0;
}
