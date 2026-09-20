# matrixclock-standing-matrix
internettime temp and webinterface to set and disable alarm<br/>

# hardware
ikea lantern holder<br/>
4 8x8 matrix<br/>
esp32<br/>
ds18b20 encapsulated in tube <br/>
pushbutton reset alarm<br/>
3 buzzers<br/>
5v power supply<br/>
fuse 0.5A<br/>
# problems
controlling the matrix was not easy (normal operating horizontal left to right)<br/>
custom char had to be made<br/>
temperature sensor ds18b20 soldered next to esp32 nok,encapasulated ds1820 with cable was the better option<br/>

# nice feature webpage with local dns<br/>
The clock synchronizes with the net via wifi.<br/>
An automatic webpage is created by wifimanager if no wifi connection.<br/>
Setting alarm via webpage. Nice is via lantaarnklok.local local dns. You do not need to know the ip of the clock.<br/>
The page works if you are on the same wifi net work and only then.<br/>
Pressbutton is on the lantern on top to reset alarm or remove the alarm.<br/>
Alarm active is shown to user by horizontal ledbar last digit.<br/>

One of my first projects together with ai gimini. The idea and making hardware from the human being me and then code by ai <br/>
and refining by testing and testing again in cooperation with ai.<br/>
