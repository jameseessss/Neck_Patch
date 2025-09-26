# ECE8873_Altium_sample

## Installing Altium
We have some specific instructions for installing, thanks to you classmate. Please see [these instructions](https://github.gatech.edu/ECE8873-Sensory-Sub-Aug/.github-private/blob/160f999e37ecde97cc4052183547c56a92e3dedc/profile/ALTIUM-Student%20License%20Request.pdf).

## Downloading sample
This is a git repository that you can clone like any other. It does not use the Altium workspace VCS. 
1. Fork this repository into the class organization
1. Use git command line tools to clone your fork on your desktop
1. In Altium, open the project (if it tells you the local copy is popinting to a VCS repository that is different from the one defined in the repository folder, PRESS CANCEL. Do not choose to get the repository from the ECE8873_sample or make the project local)
1. You can use the "Save to server" function in the project navigator to commit and push to your repository

## Working with Altium
- The main thing to start with in Altium is the schematic file and the board file (access these in the project navigation sidebar on left). In your project, you will most likely be adding additional sensors to your schematic, and then routing the board.
- Check for errors in the schematic (red zigzag lines)
- When you make changes to the schematic, go to the board file and click Design > Import changes from ...
- In the board file, you will see the footprints for the components arranged randomly. You will need to place them (move and rotate).
- There will be thin yellow lines telling you where connections need to be made. You need to route wires between each of these points (Ctrl + w). 
- The red traces are the top traces on the board, and the blue is the bottom traces. You can tunnel through the board with vias. While routing in interactive mode (Ctrl + w), switching between layers (Ctrl + Shift + scoll wheel) will automatically create a via.
- This sample contains good [design rules for PCBway](https://www.pcbway.com/capabilities.html). This is similar to the Altium .RUL file that they give [here](https://www.pcbway.com/helpcenter/design_instruction/PCBWay_Custom_Design_Rules.html), but I made some modifications.
- Check that your design passes clicking tools > Design rule check.

## Exporting for manufacturing by PCBway
- You will need to export both the gerber files and the drill files. See the guide [here](https://www.pcbway.com/blog/help_center/How_to_Generate_Gerber_and_Drill_Files_in_Altium_Designer_23_5_1_c436e1cc.html).
- The project includes samples of both the gerber and drill files.
- Make sure you look through each of the layers generated. Make sure your drills and soldermask layers show up in the right place.
