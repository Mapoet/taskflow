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
    std::cout<<"A面积="<<bg::area(A)<<"\n";
    std::cout<<"B面积="<<bg::area(B)<<"\n";
    MP R;
    bg::union_(A,B,R);
    std::cout<<"并集多边形数="<<R.size()<<"\n";
    for(size_t i=0;i<R.size();i++){
        std::cout<<"  poly["<<i<<"]面积="<<bg::area(R[i])<<" 点数="<<bg::num_points(R[i])<<"\n";
    }
    return 0;
}
