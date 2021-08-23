#!/usr/bin/env python
from xml.dom import minidom
import os

speed = 8
angle = 90

# Open the file that has grid coordinates
##with open("coordinates.txt") as f:
##   lines = f.read().splitlines()
####
#### Parse coordinates
#### Format is 'Satellite i pos: (posX, posY)'
##posX = []
##posY = []
##for line in lines:
##   posX.append(float(line[line.find("(",0,len(line))+1:line.find(", ",0,len(line))]))
##   posY.append(float(line[line.find(", ",0,len(line))+1:line.find(")",0,len(line))]))

# Create XML file
root = minidom.Document()

# Create highest level - "movements"
# <movements> ...
xmlHigherLevel = root.createElement('movements')
root.appendChild(xmlHigherLevel)

xml_str = ""    # Result string, later will be file's content
save_path_file = "mobility2.xml"  # Output file name

for i in range(0, 10):
    # Create a new movement with id='i'
    # <movement id="">...
    xml = root.createElement('movement')
    xml.setAttribute('id',str(i))
    xmlHigherLevel.appendChild(xml) # <movements> <movement id="[i]">

    # Create attributes for TurtleMobility
    # <set x="" y="" ...>
    child = root.createElement('set')
    child.setAttribute('x','uniform(3600,18900)')
    child.setAttribute('y', 'uniform(3600,18900)')
    child.setAttribute('borderPolicy',"reflect")
    xml.appendChild(child) # <movements> <movement id="[i]"> <set ...>

    # Create repeat section
    repeat = root.createElement('repeat')
    repeatChild = root.createElement('set')
    repeatChild.setAttribute('speed', 'uniform(0.5,10)')
    repeat.appendChild(repeatChild)
    repeatChild = root.createElement('turn')
    repeatChild.setAttribute('angle', 'uniform(-30,30)')
    repeat.appendChild(repeatChild)
    repeatChild = root.createElement('forward')
    repeatChild.setAttribute('t', 'uniform(0.1,1)')
    repeat.appendChild(repeatChild)
    xml.appendChild(repeat) #<movements> <movement id="[i]"> <repeat> <forward ...>

    # Update result string
    xml_str = root.toprettyxml(indent ="\t")
    
with open(save_path_file, "w") as f:
    f.write(xml_str)

for i in range(0, 10):
    print("*.terminal[" + str(i) + "].mobility.turtleScript = xmldoc(\"mobility2.xml\", \"movements//movement[@id='" + str(i) + "']\")")
print("Done!")


